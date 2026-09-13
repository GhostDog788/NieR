#include "sela/IR/Compiler.h"
#include "sela/Targets.h"
#include "../src/ir/AggregateABI.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace sela::detail;
namespace {
bool check(bool condition, llvm::StringRef message) {
  if (!condition) llvm::errs() << "semantic ABI test: " << message << '\n';
  return condition;
}
struct Fixtures {
  llvm::LLVMContext &context;
  bool wide;
  unsigned doubleAlignment;
  llvm::SmallVector<NativeABIRecordLayout, 10> records;
  llvm::Type *i8, *i32, *i64, *f32, *f64;
  llvm::StructType *floatBits, *doubleBits, *wideUnion, *floatPair, *floatUnion;
  llvm::StructType *packed, *packedPair, *bits, *crossing;
  llvm::StructType *widthUnion;

  Fixtures(llvm::LLVMContext &context, llvm::StringRef targetID)
      : context(context), wide(sela::targets::find(targetID)->wordBits == 64),
        doubleAlignment(targetID == "i686" ? 4 : 8) {
    i8 = llvm::Type::getInt8Ty(context); i32 = llvm::Type::getInt32Ty(context);
    i64 = llvm::Type::getInt64Ty(context); f32 = llvm::Type::getFloatTy(context); f64 = llvm::Type::getDoubleTy(context);
    auto storage = [&](llvm::StringRef name, llvm::ArrayRef<llvm::Type *> fields, bool packed = false) {
      if (auto *existing = llvm::StructType::getTypeByName(context, name)) return existing;
      return llvm::StructType::create(context, fields, name, packed);
    };
    floatBits = storage("union.FloatBits", {f32});
    doubleBits = storage("union.DoubleBits", {f64});
    wideUnion = storage("union.WideUnion", {llvm::ArrayType::get(f64, 2)});
    floatPair = storage("struct.FloatPair", {f32, f32});
    floatUnion = storage("union.FloatUnion", {f64});
    packed = storage("struct.Packed", {i8, i32}, true);
    packedPair = storage("struct.PackedPair", {i32, i32});
    bits = storage("struct.Bits", {i32});
    crossing = storage("struct.CrossingBits", {llvm::ArrayType::get(i8, 10)});
    widthUnion = storage("union.WidthUnion", {wide ? i64 : f64});
    auto record = [&](llvm::StructType *storageType, NativeABIRecordKind kind, uint64_t size,
                      unsigned align, llvm::ArrayRef<NativeABIFieldLayout> fields) {
      NativeABIRecordLayout value;
      value.storageType = storageType; value.kind = kind; value.sizeBytes = size;
      value.alignment = llvm::Align(align); value.fields.append(fields.begin(), fields.end());
      records.push_back(std::move(value));
    };
    record(floatBits, NativeABIRecordKind::Union, 4, 4, {{f32, 0}, {i32, 0}});
    record(doubleBits, NativeABIRecordKind::Union, 8, doubleAlignment, {{f64, 0}, {i64, 0}});
    record(wideUnion, NativeABIRecordKind::Union, 16, doubleAlignment,
           {{llvm::ArrayType::get(f64, 2), 0}, {llvm::ArrayType::get(i64, 2), 0}});
    record(floatPair, NativeABIRecordKind::Ordered, 8, 4, {{f32, 0}, {f32, 32}});
    record(floatUnion, NativeABIRecordKind::Union, 8, doubleAlignment, {{f64, 0}, {floatPair, 0}});
    record(packed, NativeABIRecordKind::Ordered, 5, 1, {{i8, 0}, {i32, 8}});
    record(packedPair, NativeABIRecordKind::Ordered, 8, 1, {{i32, 0}, {i32, 32}});
    record(bits, NativeABIRecordKind::Ordered, 4, 4, {{i32, 0, 3}, {i32, 3, 5}, {i32, 8, 24}});
    record(crossing, NativeABIRecordKind::Ordered, 10, 1, {{i64, 0, 60}, {i32, 60, 16}});
    if (wide) record(widthUnion, NativeABIRecordKind::Union, 8, 8, {{i64, 0}, {f64, 0}});
    else record(widthUnion, NativeABIRecordKind::Union, 8, doubleAlignment, {{f64, 0}});
  }
};

bool rejected(llvm::Expected<NativeABISignature> result, llvm::StringRef message) {
  if (result) return check(false, message);
  llvm::consumeError(result.takeError()); return true;
}

bool unit(llvm::StringRef targetID) {
  bool x64 = targetID == "x86_64";
  bool x86 = x64 || targetID == "i686";
  llvm::LLVMContext context;
  Fixtures fixture(context, targetID);
  auto classify = [&](llvm::StructType *type) {
    return classifyNativeLayoutABI(llvm::FunctionType::get(type, {type}, false), targetID, fixture.records);
  };
  bool passed = true;
  for (auto *record : {fixture.floatBits, fixture.doubleBits, fixture.wideUnion, fixture.floatUnion,
                       fixture.packed, fixture.packedPair, fixture.bits, fixture.crossing, fixture.widthUnion}) {
    auto result = classify(record);
    if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false; }
    if (x86 && record == fixture.packed)
      passed &= check(result->result.sRet && result->result.abiAlignment == llvm::Align(1) &&
                      result->parameters[0].byVal && result->parameters[0].abiAlignment == llvm::Align(x64 ? 8 : 4),
                      "actually unaligned packed record uses native memory ABI");
    if (x86 && record == fixture.packedPair)
      passed &= check(result->parameters[0].storageAlignment == llvm::Align(1) && !result->parameters[0].byVal &&
                      (x64 ? result->nativeType->getReturnType()->isIntegerTy(64) : result->result.abiAlignment == llvm::Align(1)),
                      "packed source alignment is explicit even when LLVM storage is not packed");
    if (x64 && record == fixture.floatBits)
      passed &= check(result->nativeType->getReturnType()->isIntegerTy(32), "union INTEGER alternative wins over float carrier");
    if (x64 && record == fixture.doubleBits)
      passed &= check(result->nativeType->getReturnType()->isIntegerTy(64), "union INTEGER alternative wins over double carrier");
    if (x64 && record == fixture.floatUnion)
      passed &= check(result->nativeType->getReturnType()->isDoubleTy(), "SSE union prefers native double carrier");
    if (x64 && record == fixture.crossing)
      passed &= check(result->result.pieces.size() == 2 && result->result.pieces[0].type->isIntegerTy(64) &&
                      result->result.pieces[1].type->isIntegerTy(16) && result->result.pieces[1].offset == 8,
                      "packed bitfield crossing eightbyte has exact i64/i16 pieces");
    if (targetID == "i686" && (record == fixture.floatBits || record == fixture.doubleBits || record == fixture.bits))
      passed &= check(result->parameters[0].byVal, "union alternatives and bitfields do not become i686 scalar field expansion");
    if (x86 && record == fixture.widthUnion)
      passed &= check(x64 ? result->parameters[0].pieces[0].type->isIntegerTy(64)
                          : result->parameters[0].kind == NativeABIKind::Expand && result->parameters[0].pieces[0].type->isDoubleTy(),
                      "selected union membership controls native carrier/class and one-member i686 expansion");
  }
  llvm::SmallVector<llvm::Type *, 8> pressure(5, fixture.i32); pressure.push_back(fixture.packed);
  auto stack = classifyNativeLayoutABI(llvm::FunctionType::get(fixture.packed, pressure, false), targetID, fixture.records);
  if (!stack) { llvm::consumeError(stack.takeError()); return false; }
  if (x64) passed &= check(stack->parameters.back().kind == NativeABIKind::Coerce &&
                          stack->parameters.back().pieces[0].type->isIntegerTy(40), "hidden sret plus five GP arguments causes exact five-byte stack coercion");

  auto signature = llvm::FunctionType::get(fixture.floatBits, {fixture.floatBits}, false);
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, {}), "storage alone cannot infer a union");
  auto invalid = fixture.records;
  invalid[0].fields[1].bitOffset = 8;
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "union alternative with nonzero offset rejects");
  invalid = fixture.records; invalid[0].kind = NativeABIRecordKind::Ordered;
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "overlap cannot masquerade as ordered fields");
  invalid = fixture.records; invalid[0].sizeBytes = 8;
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "semantic size must fit native carrier exactly");
  invalid = fixture.records; invalid[0].alignment = llvm::Align(8);
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "record size respects explicit alignment");
  invalid = fixture.records; invalid[7].fields[0].type = fixture.f32;
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "floating-point bitfield rejects");
  invalid = fixture.records; invalid[7].fields[0].bitWidth = 33;
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "bitfield wider than declared integer rejects");
  invalid = fixture.records; invalid[7].fields[1].bitOffset = 2;
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "named bitfield overlap rejects");
  invalid = fixture.records; invalid[7].fields = {{fixture.i32, 0, 32, true}};
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "all-padding record needs a separate ignored-value contract");
  invalid = fixture.records; invalid[0].fields = {{fixture.floatBits, 0}};
  passed &= rejected(classifyNativeLayoutABI(signature, targetID, invalid), "recursive semantic by-value union rejects");
  if (x64 || !x86) {
    auto crowded = fixture.records;
    llvm::Type *previous = fixture.i32;
    llvm::StructType *top = nullptr;
    for (unsigned depth = 0; depth < 16; ++depth) {
      top = llvm::StructType::create(context, {fixture.i32});
      NativeABIRecordLayout record;
      record.storageType = top; record.kind = NativeABIRecordKind::Union;
      record.sizeBytes = 4; record.alignment = llvm::Align(4);
      record.fields = {{previous, 0}, {previous, 0}, {previous, 0}};
      crowded.push_back(std::move(record)); previous = top;
    }
    passed &= rejected(classifyNativeLayoutABI(llvm::FunctionType::get(top, {top}, false), targetID, crowded),
                       "exponentially overlapping descriptor DAG has a bounded decomposition");
  }

  llvm::Module module("layout-pieces", context);
  module.setDataLayout(llvm::cantFail(nativeABIDataLayout(targetID)));
  auto *pointer = llvm::PointerType::get(context, 0);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "pieces", module);
  auto *block = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<llvm::NoFolder> builder(block);
  for (auto *record : {fixture.floatBits, fixture.doubleBits, fixture.floatUnion, fixture.packedPair, fixture.bits, fixture.crossing}) {
    auto result = classify(record);
    if (!result) { llvm::consumeError(result.takeError()); return false; }
    auto &value = result->parameters[0];
    if (value.byVal) continue;
    auto pieces = loadNativeABIPieces(builder, value, function->getArg(0), llvm::Align(1));
    if (!pieces) { llvm::logAllUnhandledErrors(pieces.takeError(), llvm::errs()); return false; }
    if (auto error = storeNativeABIPieces(builder, value, function->getArg(1), llvm::Align(1), *pieces)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs()); return false;
    }
  }
  builder.CreateRetVoid();
  passed &= check(!llvm::verifyModule(module, &llvm::errs()), "explicit-layout native piece IR verifies");
  return passed;
}

bool nativeSignature(llvm::FunctionType *native, const llvm::AttributeList &attributes,
                     const NativeABISignature &signature) {
  bool passed = check(native == signature.nativeType, "exact native signature from pinned Clang");
  if (!passed) llvm::errs() << "captured " << *native << " classified " << *signature.nativeType << '\n';
  for (unsigned i = 0; i < native->getNumParams(); ++i) {
    bool sret = signature.sretIndex && *signature.sretIndex == i;
    passed &= check(attributes.hasParamAttr(i, llvm::Attribute::StructRet) == sret, "exact sret placement");
    if (sret && attributes.hasParamAttr(i, llvm::Attribute::StructRet))
      passed &= check(attributes.getParamStructRetType(i) == signature.result.storageType &&
                      attributes.getParamAlignment(i) == signature.result.abiAlignment, "exact sret storage/alignment");
    const NativeABIValue *byval = nullptr;
    for (auto &parameter : signature.parameters)
      if (parameter.nativeBegin == i && parameter.byVal) byval = &parameter;
    passed &= check(attributes.hasParamAttr(i, llvm::Attribute::ByVal) == (byval != nullptr), "exact byval placement");
    if (byval && attributes.hasParamAttr(i, llvm::Attribute::ByVal))
      passed &= check(attributes.getParamByValType(i) == byval->storageType &&
                      attributes.getParamAlignment(i) == byval->abiAlignment, "exact byval storage/alignment");
  }
  return passed;
}

bool capture(llvm::StringRef path, llvm::StringRef targetID, bool bridge) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(path, diagnostic, context);
  if (!module) { diagnostic.print("semantic ABI fixture", llvm::errs()); return false; }
  Fixtures fixture(context, targetID);
  bool passed = check(module->getDataLayout() == llvm::cantFail(nativeABIDataLayout(targetID)), "pinned native layout");
  llvm::SmallVector<std::pair<llvm::StringRef, llvm::StructType *>, 8> entries{
      {"float_bits", fixture.floatBits}, {"double_bits", fixture.doubleBits},
      {"wide_union", fixture.wideUnion}, {"float_union", fixture.floatUnion},
      {"packed", fixture.packed}, {"packed_pair", fixture.packedPair},
      {"bits", fixture.bits}, {"crossing_bits", fixture.crossing}, {"width_union", fixture.widthUnion}};
  for (auto [name, record] : entries) {
    auto *function = module->getFunction(((bridge ? "bridge_" : "transform_") + name).str());
    if (!function) return check(false, "capture function inventory");
    llvm::SmallVector<llvm::Type *, 2> parameters;
    if (bridge) parameters.push_back(llvm::PointerType::get(context, 0));
    parameters.push_back(record);
    auto result = classifyNativeLayoutABI(llvm::FunctionType::get(record, parameters, false), targetID, fixture.records);
    if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs(), name + ": "); return false; }
    passed &= nativeSignature(function->getFunctionType(), function->getAttributes(), *result);
    if (bridge) {
      auto callback = classifyNativeLayoutABI(llvm::FunctionType::get(record, {record}, false), targetID, fixture.records);
      if (!callback) { llvm::consumeError(callback.takeError()); return false; }
      unsigned calls = 0;
      for (auto &instruction : llvm::instructions(function)) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (!call || call->getCalledFunction()) continue;
        ++calls; passed &= nativeSignature(call->getFunctionType(), call->getAttributes(), *callback);
      }
      passed &= check(calls == 1, "one native indirect callback per bridge");
    }
  }
  if (!bridge) {
    for (auto entry : {std::make_pair("packed_stack_pressure", fixture.packed),
                       std::make_pair("union_stack_pressure", fixture.wideUnion)}) {
      llvm::SmallVector<llvm::Type *, 8> parameters(5, fixture.i32); parameters.push_back(entry.second);
      if (entry.second == fixture.wideUnion) parameters.push_back(fixture.i32);
      auto result = classifyNativeLayoutABI(llvm::FunctionType::get(entry.second, parameters, false), targetID, fixture.records);
      if (!result) { llvm::consumeError(result.takeError()); return false; }
      auto *function = module->getFunction(entry.first);
      if (!function) return check(false, "capture pressure function inventory");
      passed &= nativeSignature(function->getFunctionType(), function->getAttributes(), *result);
    }
  }
  return passed;
}
} // namespace

int main(int argc, char **argv) {
  bool passed = true;
  for (auto target : sela::supportedNativeTargets()) passed &= unit(target);
  for (const auto &target : sela::targets::all()) {
    if (llvm::is_contained(sela::supportedNativeTargets(), target.id)) continue;
    llvm::LLVMContext context;
    auto *logical = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
    passed &= rejected(classifyNativeLayoutABI(logical, target.id, {}), "unavailable layout classifier rejects");
  }
  if (argc == 4) {
    llvm::StringRef target = argv[1];
    if (!llvm::is_contained(sela::supportedNativeTargets(), target)) return 2;
    passed &= capture(argv[2], target, false);
    passed &= capture(argv[3], target, true);
  } else if (argc != 1) return 2;
  return passed ? 0 : 1;
}
