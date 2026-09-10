#include "../src/ir/AggregateNormalize.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>

namespace {
std::string text(const llvm::Module &module) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  module.print(stream, nullptr);
  return output;
}
std::string canonical(llvm::Module &module) {
  llvm::StripDebugInfo(module);
  for (auto &function : module) {
    for (auto &argument : function.args()) argument.setName("");
    for (auto &block : function) {
      block.setName("");
      for (auto &instruction : block) {
        instruction.setName("");
        instruction.setMetadata(llvm::LLVMContext::MD_tbaa, nullptr);
        instruction.setMetadata(llvm::LLVMContext::MD_tbaa_struct, nullptr);
      }
    }
  }
  return text(module);
}
bool qualifiedVoidDebug() {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
define void @pointer_only(ptr %input) !dbg !4 { ret void }
!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "fixture", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "fixture.c", directory: ".")
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !DISubroutineType(types: !5)
!4 = distinct !DISubprogram(name: "pointer_only", scope: !1, file: !1, line: 1, type: !3, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!5 = !{null, !6}
!6 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!7 = !DIDerivedType(tag: DW_TAG_const_type, baseType: null)
)", diagnostic, context);
  if (!module || llvm::verifyModule(*module, &llvm::errs())) return false;
  auto before = text(*module);
  auto proof = nier::detail::normalizeNativeAggregates(*module, true);
  if (!proof) { llvm::logAllUnhandledErrors(proof.takeError(), llvm::errs()); return false; }
  return proof->functions.empty() && proof->calls.empty() && before == text(*module);
}
bool scalarResultStoredInRecord(bool wide, bool indirect, bool singleField, bool integerResult) {
  llvm::LLVMContext context;
  llvm::Module module("scalar-record-store", context);
  module.setDataLayout(nier::detail::nativeABIDataLayout(wide));
  auto *pointer = llvm::PointerType::get(context, 0);
  auto *word = llvm::IntegerType::get(context, wide ? 64 : 32);
  llvm::Type *result = integerResult ? static_cast<llvm::Type *>(word) : pointer;
  llvm::SmallVector<llvm::Type *> fields{result};
  if (!singleField) { fields.push_back(word); fields.push_back(word); }
  auto *record = llvm::StructType::create(context, fields, "record");
  auto *signature = llvm::FunctionType::get(result, {word}, false);
  auto *allocator = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage, "allocate", module);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pointer}, false),
                                         llvm::GlobalValue::ExternalLinkage, "store_scalar", module);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
  auto *storage = builder.CreateAlloca(record);
  auto *call = builder.CreateCall(signature, indirect ? static_cast<llvm::Value *>(function->getArg(0)) : allocator,
                                   {llvm::ConstantInt::get(word, 256)});
  auto *field = builder.CreateStructGEP(record, storage, 0);
  builder.CreateStore(call, field);
  builder.CreateRetVoid();
  if (llvm::verifyModule(module, &llvm::errs())) return false;
  auto before = text(module);
  auto proof = nier::detail::proveAggregateCalls(module, wide, {record});
  if (!proof) { llvm::logAllUnhandledErrors(proof.takeError(), llvm::errs()); return false; }
  return proof->empty() && before == text(module);
}
bool roundtrip(llvm::StringRef file, bool wide, llvm::StringRef output = {}) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(file, diagnostic, context);
  if (!module) return false;
  auto normalized = nier::detail::normalizeNativeAggregates(*module, wide);
  if (!normalized) { llvm::logAllUnhandledErrors(normalized.takeError(), llvm::errs(), "normalize: "); return false; }
  if (llvm::verifyModule(*module, &llvm::errs())) return false;
  auto expected = canonical(*module);
  llvm::SmallVector<llvm::StructType *> ordered;
  for (auto &function : normalized->functions)
    for (auto *record : function.orderedRecords)
      if (!llvm::is_contained(ordered, record)) ordered.push_back(record);
  for (auto &entry : normalized->calls) {
    auto native = nier::detail::classifyNativeABI(entry.second, wide, ordered);
    if (!native) { llvm::logAllUnhandledErrors(native.takeError(), llvm::errs()); return false; }
    auto call = nier::detail::materializeNativeAggregateCall(*entry.first, entry.second, *native);
    if (!call) { llvm::logAllUnhandledErrors(call.takeError(), llvm::errs()); return false; }
  }
  nier::detail::NativeABIInverseHints hints;
  for (auto &entry : normalized->functions) {
    auto function = nier::detail::materializeNativeAggregateDefinition(*entry.function, entry.logicalType, entry.native, ordered, &hints);
    if (!function) { llvm::logAllUnhandledErrors(function.takeError(), llvm::errs()); return false; }
  }
  if (llvm::verifyModule(*module, &llvm::errs())) return false;
  if (!output.empty()) {
    std::error_code error;
    llvm::raw_fd_ostream stream(output, error, llvm::sys::fs::OF_None);
    if (error) return false;
    module->print(stream, nullptr);
    stream.flush();
    if (stream.has_error()) return false;
  }
  auto inverse = nier::detail::normalizeNativeAggregates(*module, wide, &hints);
  if (!inverse) { llvm::logAllUnhandledErrors(inverse.takeError(), llvm::errs(), "inverse: "); return false; }
  if (llvm::verifyModule(*module, &llvm::errs()) || canonical(*module) != expected) {
    llvm::errs() << "regenerated native ABI failed exact normalized inverse\n";
    return false;
  }
  return true;
}
bool run(llvm::StringRef file, bool wide, bool main) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(file, diagnostic, context);
  if (!module) return false;
  auto before = text(*module);
  auto definitions = nier::detail::discoverAggregateABIs(*module, wide);
  if (!definitions) { llvm::logAllUnhandledErrors(definitions.takeError(), llvm::errs()); return false; }
  if (definitions->size() != (main ? 0u : 9u)) { llvm::errs() << file << ": definition count " << definitions->size() << '\n'; for (const auto &entry : *definitions) llvm::errs() << entry.function->getName() << '\n'; return false; }
  for (auto &definition : *definitions) {
    auto proof = nier::detail::proveAggregateDefinition(std::move(definition));
    if (!proof) { llvm::logAllUnhandledErrors(proof.takeError(), llvm::errs()); return false; }
  }
  auto calls = nier::detail::proveAggregateCalls(*module, wide);
  if (!calls) { llvm::logAllUnhandledErrors(calls.takeError(), llvm::errs()); return false; }
  if (calls->size() != (main ? 12u : 3u) || before != text(*module)) { llvm::errs() << "call count " << calls->size() << ", mutated=" << (before != text(*module)) << '\n'; return false; }
  if (!main) {
    auto normalized = nier::detail::normalizeAggregateDefinitions(*module, wide);
    if (!normalized) { llvm::logAllUnhandledErrors(normalized.takeError(), llvm::errs()); return false; }
    llvm::StripDebugInfo(*module);
    if (llvm::verifyModule(*module, &llvm::errs())) return false;
    for (auto &definition : *normalized)
      if (!definition.function->getReturnType()->isVoidTy() ||
          !definition.function->getArg(0)->getType()->isPointerTy()) return false;
  }
  return true;
}
bool definitionNegative(llvm::StringRef file, unsigned variant) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(file, diagnostic, context);
  if (!module) return false;
  auto *function = module->getFunction("pair_transform");
  llvm::StoreInst *store = nullptr;
  llvm::LoadInst *result = nullptr;
  for (auto &instruction : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        candidate && candidate->getValueOperand() == function->getArg(0)) store = candidate;
    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
      result = llvm::dyn_cast<llvm::LoadInst>(ret->getReturnValue());
  }
  if (!store || !result) return false;
  if (variant == 0) store->setVolatile(true);
  if (variant == 1) result->setVolatile(true);
  if (variant == 2) {
    llvm::IRBuilder<> builder(store);
    builder.CreateAdd(function->getArg(0), builder.getInt64(1));
  }
  if (variant == 3) store->setAlignment(llvm::Align(16));
  if (variant == 4) {
    auto *record = llvm::cast<llvm::StructType>(llvm::cast<llvm::AllocaInst>(store->getPointerOperand())->getAllocatedType());
    record->setBody({llvm::Type::getInt32Ty(context), llvm::Type::getInt64Ty(context)});
  }
  if (variant == 5) {
    auto *subprogram = function->getSubprogram();
    auto *signature = subprogram->getType();
    llvm::SmallVector<llvm::Metadata *> parameters;
    for (auto *parameter : signature->getTypeArray()) parameters.push_back(parameter);
    auto *type = llvm::cast<llvm::DIType>(parameters[1]);
    while (auto *derived = llvm::dyn_cast<llvm::DIDerivedType>(type)) type = derived->getBaseType();
    auto *record = llvm::cast<llvm::DICompositeType>(type);
    parameters[1] = llvm::DICompositeType::getDistinct(context, record->getTag(), record->getName(),
        record->getFile(), record->getLine(), record->getScope(), record->getBaseType(),
        record->getSizeInBits() + 8, record->getAlignInBits(), record->getOffsetInBits(),
        record->getFlags(), record->getElements(), record->getRuntimeLang(), record->getVTableHolder());
    subprogram->replaceType(llvm::DISubroutineType::get(context, signature->getFlags(), signature->getCC(),
        llvm::DITypeRefArray(llvm::MDTuple::get(context, parameters))));
  }
  auto before = text(*module);
  auto plans = nier::detail::normalizeAggregateDefinitions(*module, true);
  if (plans) return false;
  llvm::consumeError(plans.takeError());
  return before == text(*module);
}
bool callNegative(llvm::StringRef file, unsigned variant) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(file, diagnostic, context);
  if (!module) return false;
  llvm::CallInst *call = nullptr;
  for (auto &instruction : llvm::instructions(module->getFunction("main")))
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&instruction);
        candidate && candidate->getCalledFunction() && candidate->getCalledFunction()->getName() == "mixed_transform") call = candidate;
  if (!call) return false;
  auto *load = llvm::cast<llvm::LoadInst>(call->getArgOperand(0));
  if (variant == 0) load->setVolatile(true);
  if (variant == 1) {
    llvm::IRBuilder<> builder(call);
    builder.CreateStore(builder.getInt32(17), load->getPointerOperand());
  }
  if (variant == 2) call->addParamAttr(0, llvm::Attribute::NoUndef);
  if (variant == 3) load->setAlignment(llvm::Align(32));
  auto before = text(*module);
  auto proof = nier::detail::proveAggregateCalls(*module, true);
  if (proof) {
    // An altered piece may remain a scalar operand instead of qualifying as a
    // record pack. It must never be removed as a record shim or retain the
    // original common signature; the paired merger will then reject that
    // unmatched logical boundary.
    bool conservative = false;
    for (const auto &candidate : *proof) if (candidate.call == call) {
      conservative = candidate.logicalType->getNumParams() != 2 &&
          !llvm::is_contained(candidate.shims, load);
    }
    if (!conservative) return false;
  } else llvm::consumeError(proof.takeError());
  return before == text(*module);
}
}
int main(int argc, char **argv) {
  if (argc == 4) {
    if (llvm::StringRef(argv[2]) != "x86_64" && llvm::StringRef(argv[2]) != "i686") return 2;
    return roundtrip(argv[1], llvm::StringRef(argv[2]) == "x86_64", argv[3]) ? 0 : 1;
  }
  if (argc != 5) return 2;
  bool passed = qualifiedVoidDebug() && run(argv[1], true, false) && run(argv[2], false, false) &&
      run(argv[3], true, true) && run(argv[4], false, true);
  for (bool wide : {false, true}) for (bool indirect : {false, true})
    for (bool singleField : {false, true}) for (bool integerResult : {false, true})
      passed &= scalarResultStoredInRecord(wide, indirect, singleField, integerResult);
  for (unsigned index = 1; index <= 4; ++index) passed &= roundtrip(argv[index], index % 2);
  for (unsigned variant = 0; variant < 4; ++variant) {
    bool definitions = definitionNegative(argv[1], variant), calls = callNegative(argv[3], variant);
    if (!definitions || !calls) llvm::errs() << "negative " << variant << " definition=" << definitions << " call=" << calls << '\n';
    passed &= definitions && calls;
  }
  for (unsigned variant = 4; variant < 6; ++variant) {
    bool rejected = definitionNegative(argv[1], variant);
    if (!rejected) llvm::errs() << "forged native/debug layout was accepted: " << variant << '\n';
    passed &= rejected;
  }
  if (!passed) llvm::errs() << "aggregate storage normalization proof gate failed\n";
  return passed ? 0 : 1;
}
