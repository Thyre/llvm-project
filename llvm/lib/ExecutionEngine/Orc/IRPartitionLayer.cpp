//===----- IRPartitionLayer.cpp - Partition IR module into submodules -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/IRPartitionLayer.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IndirectionUtils.h"

using namespace llvm;
using namespace llvm::orc;

static ThreadSafeModule extractSubModule(ThreadSafeModule &TSM,
                                         StringRef Suffix,
                                         GVPredicate ShouldExtract) {

  auto DeleteExtractedDefs = [](GlobalValue &GV) {
    // Bump the linkage: this global will be provided by the external module.
    GV.setLinkage(GlobalValue::ExternalLinkage);

    // Delete the definition in the source module.
    if (isa<Function>(GV)) {
      auto &F = cast<Function>(GV);
      F.deleteBody();
      F.setPersonalityFn(nullptr);
    } else if (isa<GlobalVariable>(GV)) {
      cast<GlobalVariable>(GV).setInitializer(nullptr);
    } else if (isa<GlobalAlias>(GV)) {
      // We need to turn deleted aliases into function or variable decls based
      // on the type of their aliasee.
      auto &A = cast<GlobalAlias>(GV);
      Constant *Aliasee = A.getAliasee();
      assert(A.hasName() && "Anonymous alias?");
      assert(Aliasee->hasName() && "Anonymous aliasee");
      std::string AliasName = std::string(A.getName());

      if (isa<Function>(Aliasee)) {
        auto *F = cloneFunctionDecl(*A.getParent(), *cast<Function>(Aliasee));
        A.replaceAllUsesWith(F);
        A.eraseFromParent();
        F->setName(AliasName);
      } else if (isa<GlobalVariable>(Aliasee)) {
        auto *G = cloneGlobalVariableDecl(*A.getParent(),
                                          *cast<GlobalVariable>(Aliasee));
        A.replaceAllUsesWith(G);
        A.eraseFromParent();
        G->setName(AliasName);
      } else
        llvm_unreachable("Alias to unsupported type");
    } else
      llvm_unreachable("Unsupported global type");
  };

  auto NewTSM = cloneToNewContext(TSM, ShouldExtract, DeleteExtractedDefs);
  NewTSM.withModuleDo([&](Module &M) {
    M.setModuleIdentifier((M.getModuleIdentifier() + Suffix).str());
  });

  return NewTSM;
}

namespace llvm {
namespace orc {

class PartitioningIRMaterializationUnit : public IRMaterializationUnit {
public:
  PartitioningIRMaterializationUnit(ExecutionSession &ES,
                                    const IRSymbolMapper::ManglingOptions &MO,
                                    ThreadSafeModule TSM,
                                    IRPartitionLayer &Parent)
      : IRMaterializationUnit(ES, MO, std::move(TSM)), Parent(Parent) {}

  PartitioningIRMaterializationUnit(
      ThreadSafeModule TSM, Interface I,
      SymbolNameToDefinitionMap SymbolToDefinition, IRPartitionLayer &Parent)
      : IRMaterializationUnit(std::move(TSM), std::move(I),
                              std::move(SymbolToDefinition)),
        Parent(Parent) {}

private:
  void materialize(std::unique_ptr<MaterializationResponsibility> R) override {
    Parent.emitPartition(std::move(R), std::move(TSM),
                         std::move(SymbolToDefinition));
  }

  void discard(const JITDylib &V, const SymbolStringPtr &Name) override {
    // All original symbols were materialized by the CODLayer and should be
    // final. The function bodies provided by M should never be overridden.
    llvm_unreachable("Discard should never be called on an "
                     "ExtractingIRMaterializationUnit");
  }

  IRPartitionLayer &Parent;
};

} // namespace orc
} // namespace llvm

IRPartitionLayer::IRPartitionLayer(ExecutionSession &ES, IRLayer &BaseLayer)
    : IRLayer(ES, BaseLayer.getManglingOptions()), BaseLayer(BaseLayer) {}

void IRPartitionLayer::setPartitionFunction(PartitionFunction Partition) {
  this->Partition = Partition;
}

std::optional<IRPartitionLayer::GlobalValueSet>
IRPartitionLayer::compileRequested(GlobalValueSet Requested) {
  return std::move(Requested);
}

std::optional<IRPartitionLayer::GlobalValueSet>
IRPartitionLayer::compileWholeModule(GlobalValueSet Requested) {
  return std::nullopt;
}

void IRPartitionLayer::emit(std::unique_ptr<MaterializationResponsibility> R,
                            ThreadSafeModule TSM) {
  assert(TSM && "Null module");

  auto &ES = getExecutionSession();
  TSM.withModuleDo([&](Module &M) {
    // First, do some cleanup on the module:
    cleanUpModule(M);
  });

  // Create a partitioning materialization unit and pass the responsibility.
  if (auto Err = R->replace(std::make_unique<PartitioningIRMaterializationUnit>(
          ES, *getManglingOptions(), std::move(TSM), *this))) {
    ES.reportError(std::move(Err));
    R->failMaterialization();
    return;
  }
}

void IRPartitionLayer::cleanUpModule(Module &M) {
  for (auto &F : M.functions()) {
    if (F.isDeclaration())
      continue;

    if (F.hasAvailableExternallyLinkage()) {
      F.deleteBody();
      F.setPersonalityFn(nullptr);
      continue;
    }
  }
}

void IRPartitionLayer::expandPartition(GlobalValueSet &Partition) {
  // Expands the partition to ensure the following rules hold:
  // (1) If any alias is in the partition, its aliasee is also in the partition.
  // (2) If any aliasee is in the partition, its aliases are also in the
  //     partiton.
  // (3) If any global variable is in the partition then all global variables
  //     are in the partition.
  assert(!Partition.empty() && "Unexpected empty partition");

  const Module &M = *(*Partition.begin())->getParent();
  bool ContainsGlobalVariables = false;
  std::vector<const GlobalValue *> GVsToAdd;

  for (const auto *GV : Partition)
    if (isa<GlobalAlias>(GV))
      GVsToAdd.push_back(
          cast<GlobalValue>(cast<GlobalAlias>(GV)->getAliasee()));
    else if (isa<GlobalVariable>(GV))
      ContainsGlobalVariables = true;

  for (auto &A : M.aliases())
    if (Partition.count(cast<GlobalValue>(A.getAliasee())))
      GVsToAdd.push_back(&A);

  if (ContainsGlobalVariables)
    for (auto &G : M.globals())
      GVsToAdd.push_back(&G);

  for (const auto *GV : GVsToAdd)
    Partition.insert(GV);
}

void IRPartitionLayer::emitPartition(
    std::unique_ptr<MaterializationResponsibility> R, ThreadSafeModule TSM,
    IRMaterializationUnit::SymbolNameToDefinitionMap Defs) {

  // FIXME: Need a 'notify lazy-extracting/emitting' callback to tie the
  //        extracted module key, extracted module, and source module key
  //        together. This could be used, for example, to provide a specific
  //        memory manager instance to the linking layer.

  auto &ES = getExecutionSession();
  GlobalValueSet RequestedGVs;
  for (auto &Name : R->getRequestedSymbols()) {
    if (Name == R->getInitializerSymbol())
      TSM.withModuleDo([&](Module &M) {
        for (auto &GV : getStaticInitGVs(M))
          RequestedGVs.insert(&GV);
      });
    else {
      assert(Defs.count(Name) && "No definition for symbol");
      RequestedGVs.insert(Defs[Name]);
    }
  }

  /// Perform partitioning with the context lock held, since the partition
  /// function is allowed to access the globals to compute the partition.
  auto GVsToExtract =
      TSM.withModuleDo([&](Module &M) { return Partition(RequestedGVs); });

  // Take a 'None' partition to mean the whole module (as opposed to an empty
  // partition, which means "materialize nothing"). Emit the whole module
  // unmodified to the base layer.
  if (GVsToExtract == std::nullopt) {
    Defs.clear();
    BaseLayer.emit(std::move(R), std::move(TSM));
    return;
  }

  // If the partition is empty, return the whole module to the symbol table.
  if (GVsToExtract->empty()) {
    if (auto Err =
            R->replace(std::make_unique<PartitioningIRMaterializationUnit>(
                std::move(TSM),
                MaterializationUnit::Interface(R->getSymbols(),
                                               R->getInitializerSymbol()),
                std::move(Defs), *this))) {
      getExecutionSession().reportError(std::move(Err));
      R->failMaterialization();
      return;
    }
    return;
  }

  // Ok -- we actually need to partition the symbols. Promote the symbol
  // linkages/names, expand the partition to include any required symbols
  // (i.e. symbols that can't be separated from our partition), and
  // then extract the partition.
  //
  // FIXME: We apply this promotion once per partitioning. It's safe, but
  // overkill.
  auto ExtractedTSM = TSM.withModuleDo([&](Module &M)
                                           -> Expected<ThreadSafeModule> {
    auto PromotedGlobals = PromoteSymbols(M);
    if (!PromotedGlobals.empty()) {

      MangleAndInterner Mangle(ES, M.getDataLayout());
      SymbolFlagsMap SymbolFlags;
      IRSymbolMapper::add(ES, *getManglingOptions(), PromotedGlobals,
                          SymbolFlags);

      if (auto Err = R->defineMaterializing(SymbolFlags))
        return std::move(Err);
    }

    expandPartition(*GVsToExtract);

    // Submodule name is given by hashing the names of the globals.
    std::string SubModuleName;
    {
      std::vector<const GlobalValue *> HashGVs;
      HashGVs.reserve(GVsToExtract->size());
      llvm::append_range(HashGVs, *GVsToExtract);
      llvm::sort(HashGVs, [](const GlobalValue *LHS, const GlobalValue *RHS) {
        return LHS->getName() < RHS->getName();
      });
      hash_code HC(0);
      for (const auto *GV : HashGVs) {
        assert(GV->hasName() && "All GVs to extract should be named by now");
        auto GVName = GV->getName();
        HC = hash_combine(HC, hash_combine_range(GVName));
      }
      raw_string_ostream(SubModuleName)
          << ".submodule."
          << formatv(sizeof(size_t) == 8 ? "{0:x16}" : "{0:x8}",
                     static_cast<size_t>(HC))
          << ".ll";
    }

    // Extract the requested partiton (plus any necessary aliases) and
    // put the rest back into the impl dylib.
    auto ShouldExtract = [&](const GlobalValue &GV) -> bool {
      return GVsToExtract->count(&GV);
    };

    return extractSubModule(TSM, SubModuleName, ShouldExtract);
  });

  if (!ExtractedTSM) {
    ES.reportError(ExtractedTSM.takeError());
    R->failMaterialization();
    return;
  }

  if (auto Err = R->replace(std::make_unique<PartitioningIRMaterializationUnit>(
          ES, *getManglingOptions(), std::move(TSM), *this))) {
    ES.reportError(std::move(Err));
    R->failMaterialization();
    return;
  }
  BaseLayer.emit(std::move(R), std::move(*ExtractedTSM));
}
