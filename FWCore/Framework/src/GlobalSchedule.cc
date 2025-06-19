#include "FWCore/Framework/interface/GlobalSchedule.h"
#include "FWCore/Framework/interface/maker/ModuleMaker.h"
#include "FWCore/Framework/src/TriggerResultInserter.h"
#include "FWCore/Framework/src/PathStatusInserter.h"
#include "FWCore/Framework/src/EndPathStatusInserter.h"
#include "FWCore/Framework/interface/PreallocationConfiguration.h"

#include "DataFormats/Provenance/interface/ProcessConfiguration.h"
#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/Utilities/interface/Algorithms.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/ExceptionCollector.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>

namespace edm {
  GlobalSchedule::GlobalSchedule(
      std::shared_ptr<TriggerResultInserter> inserter,
      std::vector<edm::propagate_const<std::shared_ptr<PathStatusInserter>>>& pathStatusInserters,
      std::vector<edm::propagate_const<std::shared_ptr<EndPathStatusInserter>>>& endPathStatusInserters,
      std::shared_ptr<ModuleRegistry> modReg,
      std::vector<std::string> const& iModulesToUse,
      ParameterSet& proc_pset,
      SignallingProductRegistryFiller& pregistry,
      PreallocationConfiguration const& prealloc,
      ExceptionToActionTable const& actions,
      std::shared_ptr<ActivityRegistry> areg,
      std::shared_ptr<ProcessConfiguration const> processConfiguration,
      ProcessContext const* processContext)
      : actReg_(areg),
        processContext_(processContext),
        numberOfConcurrentLumis_(prealloc.numberOfLuminosityBlocks()),
        numberOfConcurrentRuns_(prealloc.numberOfRuns()) {
    unsigned int nManagers = prealloc.numberOfLuminosityBlocks() + prealloc.numberOfRuns() +
                             numberOfConcurrentProcessBlocks_ + numberOfConcurrentJobs_;
    workerManagers_.reserve(nManagers);
    for (unsigned int i = 0; i < nManagers; ++i) {
      workerManagers_.emplace_back(modReg, areg, actions);
    }
    for (auto const& moduleLabel : iModulesToUse) {
      bool isTracked;
      ParameterSet* modpset = proc_pset.getPSetForUpdate(moduleLabel, isTracked);
      if (modpset != nullptr) {  // It will be null for PathStatusInserters, it should
                                 // be impossible to be null for anything else
        assert(isTracked);

        //side effect keeps this module around
        for (auto& wm : workerManagers_) {
          (void)wm.getWorker(*modpset, pregistry, &prealloc, processConfiguration, moduleLabel);
        }
      }
    }
    if (inserter) {
      for (auto& wm : workerManagers_) {
        (void)wm.getWorkerForModule(*inserter);
      }
    }

    for (auto& pathStatusInserter : pathStatusInserters) {
      std::shared_ptr<PathStatusInserter> inserterPtr = get_underlying(pathStatusInserter);

      for (auto& wm : workerManagers_) {
        (void)wm.getWorkerForModule(*inserterPtr);
      }
    }

    for (auto& endPathStatusInserter : endPathStatusInserters) {
      std::shared_ptr<EndPathStatusInserter> inserterPtr = get_underlying(endPathStatusInserter);
      for (auto& wm : workerManagers_) {
        (void)wm.getWorkerForModule(*inserterPtr);
      }
    }

  }  // GlobalSchedule::GlobalSchedule

  void GlobalSchedule::beginJob(ProcessContext const& processContext) {
    GlobalContext globalContext(GlobalContext::Transition::kBeginJob, processContext_);
    unsigned int const managerIndex =
        numberOfConcurrentLumis_ + numberOfConcurrentRuns_ + numberOfConcurrentProcessBlocks_;

    std::exception_ptr exceptionPtr;
    CMS_SA_ALLOW try {
      try {
        convertException::wrap([this, &processContext]() { actReg_->preBeginJobSignal_(processContext); });
      } catch (cms::Exception& ex) {
        exceptionContext(ex, globalContext, "Handling pre signal, likely in a service function");
        throw;
      }
      workerManagers_[managerIndex].beginJob(globalContext);
    } catch (...) {
      exceptionPtr = std::current_exception();
    }

    try {
      convertException::wrap([this]() { actReg_->postBeginJobSignal_(); });
    } catch (cms::Exception& ex) {
      if (!exceptionPtr) {
        exceptionContext(ex, globalContext, "Handling post signal, likely in a service function");
        exceptionPtr = std::current_exception();
      }
    }
    if (exceptionPtr) {
      std::rethrow_exception(exceptionPtr);
    }
  }

  void GlobalSchedule::endJob(ExceptionCollector& collector) {
    GlobalContext globalContext(GlobalContext::Transition::kEndJob, processContext_);
    unsigned int const managerIndex =
        numberOfConcurrentLumis_ + numberOfConcurrentRuns_ + numberOfConcurrentProcessBlocks_;

    std::exception_ptr exceptionPtr;
    CMS_SA_ALLOW try {
      try {
        convertException::wrap([this]() { actReg_->preEndJobSignal_(); });
      } catch (cms::Exception& ex) {
        exceptionContext(ex, globalContext, "Handling pre signal, likely in a service function");
        throw;
      }
      workerManagers_[managerIndex].endJob(collector, globalContext);
    } catch (...) {
      exceptionPtr = std::current_exception();
    }

    try {
      convertException::wrap([this]() { actReg_->postEndJobSignal_(); });
    } catch (cms::Exception& ex) {
      if (!exceptionPtr) {
        exceptionContext(ex, globalContext, "Handling post signal, likely in a service function");
        exceptionPtr = std::current_exception();
      }
    }
    if (exceptionPtr) {
      collector.call([&exceptionPtr]() { std::rethrow_exception(exceptionPtr); });
    }
  }

  void GlobalSchedule::replaceModule(maker::ModuleHolder* iMod, std::string const& iLabel) {
    Worker* found = nullptr;
    unsigned int const jobManagerIndex =
        numberOfConcurrentLumis_ + numberOfConcurrentRuns_ + numberOfConcurrentProcessBlocks_;
    unsigned int managerIndex = 0;
    for (auto& wm : workerManagers_) {
      for (auto const& worker : wm.allWorkers()) {
        if (worker->description()->moduleLabel() == iLabel) {
          found = worker;
          break;
        }
      }
      if (nullptr == found) {
        return;
      }

      iMod->replaceModuleFor(found);
      if (managerIndex == jobManagerIndex) {
        GlobalContext globalContext(GlobalContext::Transition::kBeginJob, processContext_);
        found->beginJob(globalContext);
      }
      ++managerIndex;
    }
  }

  void GlobalSchedule::deleteModule(std::string const& iLabel) {
    for (auto& wm : workerManagers_) {
      wm.deleteModuleIfExists(iLabel);
    }
  }

  std::vector<ModuleDescription const*> GlobalSchedule::getAllModuleDescriptions() const {
    std::vector<ModuleDescription const*> result;
    result.reserve(allWorkers().size());

    for (auto const& worker : allWorkers()) {
      ModuleDescription const* p = worker->description();
      result.push_back(p);
    }
    return result;
  }

  void GlobalSchedule::handleException(GlobalContext const* globalContext,
                                       ServiceWeakToken const& weakToken,
                                       bool cleaningUpAfterException,
                                       std::exception_ptr& excpt) {
    //add context information to the exception and print message
    try {
      convertException::wrap([&excpt]() { std::rethrow_exception(excpt); });
    } catch (cms::Exception& ex) {
      std::ostringstream ost;
      // In most cases the exception will already have context at this point,
      // but add some context here in those rare cases where it does not.
      if (ex.context().empty()) {
        exceptionContext(ost, *globalContext);
      }
      ServiceRegistry::Operate op(weakToken.lock());
      addContextAndPrintException(ost.str().c_str(), ex, cleaningUpAfterException);
      excpt = std::current_exception();
    }
    // We are already handling an earlier exception, so ignore it
    // if this signal results in another exception being thrown.
    CMS_SA_ALLOW try {
      if (actReg_) {
        ServiceRegistry::Operate op(weakToken.lock());
        actReg_->preGlobalEarlyTerminationSignal_(*globalContext, TerminationOrigin::ExceptionFromThisContext);
      }
    } catch (...) {
    }
  }

}  // namespace edm
