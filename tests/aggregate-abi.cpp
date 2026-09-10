#include "../src/ir/AggregateABI.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace nier::detail;
namespace {
bool check(bool condition, llvm::StringRef message) {
  if (!condition) llvm::errs() << "aggregate ABI test: " << message << '\n';
  return condition;
}

bool rejected(llvm::Expected<NativeABISignature> result, llvm::StringRef message) {
  if (result) return check(false, message);
  llvm::consumeError(result.takeError());
  return true;
}

bool descriptorTests(bool x64) {
  llvm::LLVMContext context;
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *word = llvm::IntegerType::get(context, x64 ? 64 : 32);
  auto *f32 = llvm::Type::getFloatTy(context);
  auto *f64 = llvm::Type::getDoubleTy(context);
  auto *pair = llvm::StructType::create(context, {i32, i32});
  auto *mixed = llvm::StructType::create(context, {f64, i32});
  auto *large = llvm::StructType::create(context, {f64, f64, word});
  auto *floats = llvm::StructType::create(context, {f32, f32});
  auto *twoWords = llvm::StructType::create(context, {word, word});
  auto *small = llvm::StructType::create(context, {llvm::ArrayType::get(i8, 3)});
  llvm::SmallVector<llvm::StructType *, 8> records{pair, mixed, large, floats, twoWords, small};
  bool passed = true;
  for (auto *record : records) {
    auto result = classifyNativeABI(llvm::FunctionType::get(record, {record}, false), x64, records);
    if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false; }
    passed &= check(result->result.sRet == (!x64 || record == large), "native hidden aggregate result");
    passed &= check(result->parameters[0].nativeBegin == (result->sretIndex ? 1u : 0u), "native argument starts after sret");
    if (record == pair && x64)
      passed &= check(result->nativeType->getReturnType()->isIntegerTy(64) &&
                      result->parameters[0].pieces[0].type->isIntegerTy(64), "Pair uses one i64 on x64");
    if (record == mixed && x64)
      passed &= check(result->result.pieces.size() == 2 && result->result.pieces[1].offset == 8 &&
                      result->result.pieces[1].type->isIntegerTy(32), "Mixed excludes trailing padding from high piece");
    if (record == floats && x64)
      passed &= check(result->result.pieces[0].type == llvm::FixedVectorType::get(f32, 2), "TwoFloats uses one SSE vector");
    if (record == large)
      passed &= check(result->parameters[0].byVal && result->parameters[0].abiAlignment == llvm::Align(x64 ? 8 : 4), "Large native byval alignment");
    if (record == small && x64)
      passed &= check(result->parameters[0].pieces[0].type->isIntegerTy(24), "Three-byte integer coercion is exact width");
  }

  llvm::SmallVector<llvm::Type *, 16> pressure(6, i32);
  pressure.append(8, f64);
  pressure.push_back(floats);
  auto stack = classifyNativeABI(llvm::FunctionType::get(floats, pressure, false), x64, records);
  if (!stack) { llvm::logAllUnhandledErrors(stack.takeError(), llvm::errs()); return false; }
  if (x64)
    passed &= check(stack->parameters.back().kind == NativeABIKind::Coerce &&
                    stack->parameters.back().pieces[0].type->isIntegerTy(64), "exhausted banks use Clang's no-GP scalar-stack coercion");
  pressure.assign(8, f64); pressure.push_back(floats);
  auto sse = classifyNativeABI(llvm::FunctionType::get(floats, pressure, false), x64, records);
  if (!sse) { llvm::logAllUnhandledErrors(sse.takeError(), llvm::errs()); return false; }
  if (x64) passed &= check(sse->parameters.back().byVal && sse->remainingGP == 6, "SSE exhaustion alone forces byval without consuming GP");
  pressure.assign(5, i32); pressure.push_back(twoWords); pressure.push_back(i32);
  auto rollback = classifyNativeABI(llvm::FunctionType::get(twoWords, pressure, false), x64, records);
  if (!rollback) { llvm::logAllUnhandledErrors(rollback.takeError(), llvm::errs()); return false; }
  if (x64) passed &= check(rollback->parameters[5].byVal && rollback->remainingGP == 0, "whole-aggregate rollback leaves last GP for tail scalar");
  pressure.assign(5, i32); pressure.push_back(mixed);
  auto hidden = classifyNativeABI(llvm::FunctionType::get(large, pressure, false), x64, records);
  if (!hidden) { llvm::logAllUnhandledErrors(hidden.takeError(), llvm::errs()); return false; }
  if (x64) passed &= check(hidden->parameters.back().byVal && hidden->remainingSSE == 8, "hidden result consumes a GP before parameters");

  passed &= rejected(classifyNativeABI(llvm::FunctionType::get(pair, {pair}, false), x64, {}),
                     "unqualified union-like LLVM storage cannot become an ABI record");
  auto *packed = llvm::StructType::get(context, {i8, i32}, true);
  passed &= rejected(classifyNativeABI(llvm::FunctionType::get(packed, {packed}, false), x64, {packed}), "packed layout rejects");
  auto *empty = llvm::StructType::get(context);
  passed &= rejected(classifyNativeABI(llvm::FunctionType::get(empty, {empty}, false), x64, {empty}), "empty layout rejects");
  auto *vector = llvm::StructType::get(context, llvm::ArrayRef<llvm::Type *>{llvm::FixedVectorType::get(f32, 4)});
  passed &= rejected(classifyNativeABI(llvm::FunctionType::get(vector, {vector}, false), x64, {vector}), "unqualified vector field rejects");
  auto *oversized = llvm::StructType::get(context, llvm::ArrayRef<llvm::Type *>{
      llvm::ArrayType::get(llvm::Type::getInt64Ty(context), 1ULL << 62)});
  passed &= rejected(classifyNativeABI(llvm::FunctionType::get(oversized, {oversized}, false), x64, {oversized}),
                     "oversized array rejects before target size multiplication can overflow");

  llvm::Module module("pieces", context);
  module.setDataLayout(nativeABIDataLayout(x64));
  auto *pointer = llvm::PointerType::get(context, 0);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "copy_pieces", module);
  auto *block = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<llvm::NoFolder> builder(block);
  for (auto *record : {pair, mixed, floats, small}) {
    auto signature = classifyNativeABI(llvm::FunctionType::get(record, {record}, false), x64, records);
    if (!signature) { llvm::consumeError(signature.takeError()); return false; }
    auto descriptor = signature->parameters[0];
    if (descriptor.byVal) continue;
    auto pieces = loadNativeABIPieces(builder, descriptor, function->getArg(0), llvm::Align(1));
    if (!pieces) { llvm::logAllUnhandledErrors(pieces.takeError(), llvm::errs()); return false; }
    if (auto error = storeNativeABIPieces(builder, descriptor, function->getArg(1), llvm::Align(1), *pieces)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs()); return false;
    }
    size_t before = block->size();
    auto invalid = descriptor;
    invalid.pieces.back().offset = invalid.storageSize;
    auto bad = loadNativeABIPieces(builder, invalid, function->getArg(0), llvm::Align(1));
    if (bad) passed &= check(false, "out-of-bounds load rejects"); else llvm::consumeError(bad.takeError());
    passed &= check(block->size() == before, "failed piece validation emits no partial IR");
    auto wrong = *pieces;
    wrong.back() = llvm::ConstantInt::get(i8, 0);
    auto error = storeNativeABIPieces(builder, descriptor, function->getArg(1), llvm::Align(1), wrong);
    if (error) llvm::consumeError(std::move(error)); else passed &= check(false, "wrong piece type rejects");
    passed &= check(block->size() == before, "failed store validation emits no partial IR");
  }
  builder.CreateRetVoid();
  for (auto &instruction : *block) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
      passed &= check(load->getAlign() == llvm::Align(1), "loads honor conservative base alignment");
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
      passed &= check(store->getAlign() == llvm::Align(1) && llvm::isa<llvm::LoadInst>(store->getValueOperand()),
                      "stores copy only loaded native pieces, never invent padding zeros");
  }
  passed &= check(!llvm::verifyModule(module, &llvm::errs()), "generated piece IR verifies");
  return passed;
}

bool captureTests(llvm::StringRef path, bool x64, bool extra) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(path, diagnostic, context);
  if (!module) { diagnostic.print("aggregate ABI fixture", llvm::errs()); return false; }
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *f64 = llvm::Type::getDoubleTy(context);
  auto *pointer = llvm::PointerType::get(context, 0);
  auto *pair = llvm::StructType::getTypeByName(context, "struct.Pair");
  auto *mixed = llvm::StructType::getTypeByName(context, "struct.Mixed");
  auto *large = llvm::StructType::getTypeByName(context, "struct.Large");
  auto *floats = llvm::StructType::getTypeByName(context, "struct.TwoFloats");
  auto *words = llvm::StructType::getTypeByName(context, "struct.TwoWords");
  auto *bytes = llvm::StructType::getTypeByName(context, "struct.ThreeBytes");
  auto *nested = llvm::StructType::getTypeByName(context, "struct.Nested");
  llvm::SmallVector<llvm::StructType *, 8> records;
  for (auto *record : {pair, mixed, large, floats, words, bytes, nested})
    if (record) records.push_back(record);
  if (!pair || !mixed || !large || (extra && (!floats || !words || !bytes || !nested)))
    return check(false, "fixture logical record inventory");
  bool passed = check(module->getDataLayout() == nativeABIDataLayout(x64), "capture uses pinned target layout");
  auto compare = [&](llvm::StringRef name, llvm::Type *resultType, llvm::ArrayRef<llvm::Type *> arguments) {
    auto *function = module->getFunction(name);
    if (!function) { passed &= check(false, "fixture function inventory"); return; }
    auto signature = classifyNativeABI(llvm::FunctionType::get(resultType, arguments, false), x64, records);
    if (!signature) { llvm::logAllUnhandledErrors(signature.takeError(), llvm::errs(), name + ": "); passed = false; return; }
    if (signature->nativeType != function->getFunctionType()) {
      llvm::errs() << name << " expected " << *function->getFunctionType() << " classified " << *signature->nativeType << '\n';
      passed = false;
    }
    for (unsigned i = 0; i < function->arg_size(); ++i) {
      auto attrs = function->getAttributes();
      bool sret = signature->sretIndex && *signature->sretIndex == i;
      passed &= check(attrs.hasParamAttr(i, llvm::Attribute::StructRet) == sret, "capture sret placement");
      if (sret)
        passed &= check(attrs.getParamStructRetType(i) == signature->result.storageType &&
                        attrs.getParamAlignment(i) == signature->result.abiAlignment, "capture sret storage type/alignment");
      const NativeABIValue *byval = nullptr;
      for (auto &parameter : signature->parameters)
        if (parameter.nativeBegin == i && parameter.byVal) byval = &parameter;
      passed &= check(attrs.hasParamAttr(i, llvm::Attribute::ByVal) == (byval != nullptr), "capture byval placement");
      if (byval)
        passed &= check(attrs.getParamByValType(i) == byval->storageType &&
                        attrs.getParamAlignment(i) == byval->abiAlignment, "capture byval storage type/alignment");
    }
  };
  if (!extra) {
    compare("pair_transform", pair, {pair, i32});
    compare("mixed_transform", mixed, {mixed, i32});
    compare("large_transform", large, {large, i32});
    compare("pair_indirect", pair, {pointer, pair, i32});
    compare("mixed_indirect", mixed, {pointer, mixed, i32});
    compare("large_indirect", large, {pointer, large, i32});
    for (auto entry : {std::make_pair("pair_indirect", pair),
                       std::make_pair("mixed_indirect", mixed),
                       std::make_pair("large_indirect", large)}) {
      auto signature = classifyNativeABI(llvm::FunctionType::get(entry.second, {entry.second, i32}, false), x64, records);
      if (!signature) { llvm::consumeError(signature.takeError()); return false; }
      unsigned count = 0;
      for (auto &instruction : llvm::instructions(module->getFunction(entry.first))) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (!call || call->getCalledFunction()) continue;
        ++count;
        passed &= check(call->getFunctionType() == signature->nativeType,
                        "indirect native call uses logical callback signature, not enclosing function register pressure");
        for (unsigned i = 0; i < call->arg_size(); ++i) {
          auto attrs = call->getAttributes();
          bool sret = signature->sretIndex && *signature->sretIndex == i;
          passed &= check(attrs.hasParamAttr(i, llvm::Attribute::StructRet) == sret,
                          "indirect native call sret placement");
          if (sret)
            passed &= check(attrs.getParamStructRetType(i) == signature->result.storageType &&
                            attrs.getParamAlignment(i) == signature->result.abiAlignment,
                            "indirect call sret type/alignment are independently checked");
          for (auto &parameter : signature->parameters)
            if (parameter.nativeBegin == i && parameter.byVal)
              passed &= check(attrs.getParamByValType(i) == parameter.storageType &&
                              attrs.getParamAlignment(i) == parameter.abiAlignment,
                              "indirect call byval type/alignment are independently checked");
        }
      }
      passed &= check(count == 1, "indirect callback capture inventory");
    }
    llvm::SmallVector<llvm::Type *, 16> pressure(5, i32);
    pressure.push_back(pair); pressure.push_back(i32);
    compare("pair_register_edge", pair, pressure);
    pressure.insert(pressure.begin(), i32);
    compare("pair_stack_edge", pair, pressure);
    pressure.assign(6, i32); pressure.append(8, f64); pressure.push_back(mixed); pressure.push_back(i32);
    compare("mixed_stack_edge", mixed, pressure);
  } else {
    compare("two_floats", floats, {floats});
    llvm::SmallVector<llvm::Type *, 16> pressure(8, f64); pressure.push_back(floats);
    compare("float_bank_full", floats, pressure);
    pressure.assign(6, i32); pressure.append(8, f64); pressure.push_back(floats);
    compare("both_banks_full", floats, pressure);
    pressure.assign(5, i32); pressure.push_back(mixed);
    compare("hidden_result_pressure", large, pressure);
    pressure.assign(5, i32); pressure.push_back(words); pressure.push_back(i32);
    compare("rollback_gp", words, pressure);
    compare("three_bytes", bytes, {bytes});
    compare("nested_record", nested, {nested});
  }
  return passed;
}
} // namespace

int main(int argc, char **argv) {
  bool passed = descriptorTests(true) && descriptorTests(false);
  if (argc == 5) {
    passed &= captureTests(argv[1], true, false);
    passed &= captureTests(argv[2], false, false);
    passed &= captureTests(argv[3], true, true);
    passed &= captureTests(argv[4], false, true);
  } else if (argc != 1) return 2;
  return passed ? 0 : 1;
}
