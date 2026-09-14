#include "InstructionContracts.h"
#include "sela/IR/Dialect.h"
#include "sela/IR/Intrinsics.h"
#include "sela/IR/Domains.h"
#include "sela/Targets.h"
#include "mlir/IR/BuiltinTypes.h"
#include "llvm/IR/InlineAsm.h"

namespace sela::detail {
namespace {
llvm::Error invalid(llvm::StringRef message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), message);
}
bool integer(mlir::Type type) {
  if (auto vector = mlir::dyn_cast<mlir::VectorType>(type)) type = vector.getElementType();
  return mlir::isa<mlir::IntegerType>(type);
}
mlir::IntegerAttr immediate(mlir::Value value) {
  auto *definition = value.getDefiningOp();
  return definition && definition->getName().getStringRef() == "sela.constant"
      ? definition->getAttrOfType<mlir::IntegerAttr>("value") : mlir::IntegerAttr();
}
bool immediateIn(mlir::Value value, uint64_t maximum) {
  auto number = immediate(value);
  auto type = mlir::dyn_cast<mlir::IntegerType>(value.getType());
  if (!number || !type) return false;
  auto bits = number.getValue().sextOrTrunc(type.getWidth());
  return bits.getActiveBits() <= 64 && bits.getZExtValue() <= maximum;
}
mlir::Type indexedType(mlir::Type type, mlir::DenseI32ArrayAttr indices) {
  if (!indices || indices.empty()) return {};
  for (int32_t index : indices.asArrayRef()) {
    if (index < 0) return {};
    if (auto record = mlir::dyn_cast<ir::RecordType>(type)) {
      if (unsigned(index) >= record.getFields().size()) return {};
      type = record.getFields()[index];
    } else if (auto array = mlir::dyn_cast<ir::ArrayType>(type)) {
      if (uint64_t(index) >= array.getNumElements()) return {};
      type = array.getElementType();
    } else return {};
  }
  return type;
}
}
llvm::Error validateLoopOption(mlir::Attribute option) {
  if (auto name = mlir::dyn_cast<mlir::StringAttr>(option)) {
    if (name.getValue() == "llvm.loop.mustprogress" || name.getValue() == "llvm.loop.unroll.disable" ||
        name.getValue() == "llvm.loop.unroll.enable") return llvm::Error::success();
  }
  if (auto record = mlir::dyn_cast<mlir::DictionaryAttr>(option)) {
    auto name = record.getAs<mlir::StringAttr>("name");
    auto value = record.getAs<mlir::IntegerAttr>("value");
    if (record.size() == 2 && name && value && value.getType().isInteger(32) &&
        value.getInt() > 0 && value.getInt() <= 65536 &&
        (name.getValue() == "llvm.loop.vectorize.width" || name.getValue() == "llvm.loop.interleave.count" ||
         name.getValue() == "llvm.loop.unroll.count")) return llvm::Error::success();
  }
  return invalid("unknown loop semantic option");
}
llvm::Error validateExtendedInstruction(mlir::Operation &op) {
  auto name = op.getName().getStringRef();
  if (name == "sela.extract_value" || name == "sela.insert_value") {
    const bool insert = name == "sela.insert_value";
    if (op.getNumOperands() != (insert ? 2U : 1U) || op.getNumResults() != 1)
      return invalid("invalid aggregate SSA arity");
    auto selected = indexedType(op.getOperand(0).getType(), op.getAttrOfType<mlir::DenseI32ArrayAttr>("indices"));
    if (!selected || selected != (insert ? op.getOperand(1).getType() : op.getResult(0).getType()) ||
        (insert && op.getResult(0).getType() != op.getOperand(0).getType()))
      return invalid("invalid aggregate SSA index or type");
  } else if (name == "sela.extract_element" || name == "sela.insert_element") {
    const bool insert = name == "sela.insert_element";
    if (op.getNumOperands() != (insert ? 3U : 2U) || op.getNumResults() != 1)
      return invalid("invalid vector element arity");
    auto vector = mlir::dyn_cast<mlir::VectorType>(op.getOperand(0).getType());
    if (!vector || !mlir::isa<mlir::IntegerType>(op.getOperand(insert ? 2 : 1).getType()) ||
        vector.getElementType() != (insert ? op.getOperand(1).getType() : op.getResult(0).getType()) ||
        (insert && vector != op.getResult(0).getType()))
      return invalid("invalid vector element types");
    // Out-of-range dynamic and constant indices retain LLVM's poison semantics.
  } else if (name == "sela.shuffle") {
    if (op.getNumOperands() != 2 || op.getNumResults() != 1) return invalid("invalid vector shuffle arity");
    auto input = mlir::dyn_cast<mlir::VectorType>(op.getOperand(0).getType());
    auto output = mlir::dyn_cast<mlir::VectorType>(op.getResult(0).getType());
    auto mask = op.getAttrOfType<mlir::DenseI32ArrayAttr>("mask");
    if (!input || !output || input != op.getOperand(1).getType() ||
        input.getElementType() != output.getElementType() || !mask || mask.size() != output.getNumElements())
      return invalid("invalid vector shuffle types or mask length");
    for (int32_t lane : mask.asArrayRef())
      if (lane < -1 || lane >= input.getNumElements() * 2) return invalid("invalid vector shuffle lane");
  } else if (name == "sela.inline_asm" || name == "sela.inline_asm_br") {
    const bool branch = name == "sela.inline_asm_br";
    size_t argumentCount = op.getNumOperands();
    if (branch) {
      auto arguments = op.getAttrOfType<mlir::IntegerAttr>("argument_count");
      auto counts = op.getAttrOfType<mlir::DenseI32ArrayAttr>("argument_counts");
      if (!arguments || arguments.getValue().getBitWidth() > 64 || arguments.getInt() < 0 ||
          uint64_t(arguments.getInt()) > op.getNumOperands() || !counts || counts.size() != op.getNumSuccessors() ||
          op.getNumSuccessors() < 2) return invalid("invalid assembly branch argument inventory");
      argumentCount = arguments.getInt();
      size_t offset = argumentCount;
      for (unsigned index = 0; index < counts.size(); ++index) {
        auto count = counts[index];
        auto *successor = op.getSuccessor(index);
        if (count < 0 || size_t(count) > op.getNumOperands() - offset || size_t(count) != successor->getNumArguments())
          return invalid("invalid assembly branch edge arguments");
        for (int32_t i = 0; i < count; ++i)
          if (op.getOperand(offset + i).getType() != successor->getArgument(i).getType())
            return invalid("assembly branch edge type mismatch");
        offset += count;
      }
      if (offset != op.getNumOperands()) return invalid("undeclared assembly branch edge operands");
    } else if (op.getNumSuccessors()) return invalid("ordinary inline assembly cannot have successors");
    auto text = op.getAttrOfType<mlir::StringAttr>("template");
    auto constraints = op.getAttrOfType<mlir::StringAttr>("constraints");
    auto backend = op.getAttrOfType<mlir::StringAttr>("backend");
    auto dialect = op.getAttrOfType<mlir::StringAttr>("dialect");
    auto tail = op.getAttrOfType<mlir::IntegerAttr>("tail");
    if (op.getNumResults() > 1 || !text || !constraints || !backend || !dialect ||
        (dialect.getValue() != "att" && dialect.getValue() != "intel") ||
        !tail || tail.getValue().getBitWidth() > 64 || tail.getInt() < 0 || tail.getInt() > 3 ||
        text.getValue().size() > 1024 * 1024 || constraints.getValue().size() > 65536 ||
        text.getValue().contains('\0') || constraints.getValue().contains('\0')) return invalid("invalid inline assembly record");
    for (auto field : {"side_effects", "align_stack", "can_throw"})
      if (!op.getAttrOfType<mlir::BoolAttr>(field)) return invalid("invalid inline assembly effect flag");
    auto targets = ir::declaredTargets(op.getParentOfType<mlir::ModuleOp>());
    if (!targets) return targets.takeError();
    for (auto target : *targets)
      if (targets::find(target)->llvmBackend != backend.getValue()) return invalid("inline assembly is outside its target backend domain");
    auto parsed = llvm::InlineAsm::ParseConstraints(constraints.getValue());
    if (parsed.empty() && !constraints.getValue().empty()) return invalid("invalid inline assembly constraints");
    unsigned arguments = 0, outputs = 0, labels = 0;
    for (const auto &entry : parsed) {
      arguments += entry.hasArg();
      outputs += entry.Type == llvm::InlineAsm::isOutput && !entry.isIndirect;
      labels += entry.Type == llvm::InlineAsm::isLabel;
    }
    unsigned results = op.getNumResults();
    if (results) if (auto record = mlir::dyn_cast<ir::RecordType>(op.getResult(0).getType())) results = record.getFields().size();
    if (arguments != argumentCount || outputs != results || labels != (branch ? op.getNumSuccessors() - 1 : 0) ||
        (branch && tail.getInt() != 0)) return invalid("inline assembly constraint arity mismatch");
  } else if (name == "sela.intrinsic") {
    auto id = op.getAttrOfType<mlir::StringAttr>("name");
    auto *spec = id ? ir::findIntrinsic(id.getValue()) : nullptr;
    if (!spec || op.getNumOperands() != spec->operands || op.getNumResults() > 1)
      return invalid("unknown or malformed registered Sela intrinsic");
    auto first = op.getOperand(0).getType();
    auto result = op.getNumResults() ? op.getResult(0).getType() : mlir::Type();
    auto sameArguments = [&](unsigned count) {
      for (unsigned index = 1; index < count; ++index) if (op.getOperand(index).getType() != first) return false;
      return true;
    };
    auto key = spec->name;
    if (key == "assume") {
      if (!first.isInteger(1) || result) return invalid("assume requires an i1 and no result");
    } else if (key == "prefetch") {
      if (!mlir::isa<ir::PointerType>(first) || result ||
          !op.getOperand(1).getType().isInteger(32) || !op.getOperand(2).getType().isInteger(32) ||
          !op.getOperand(3).getType().isInteger(32) || !immediateIn(op.getOperand(1), 1) ||
          !immediateIn(op.getOperand(2), 3) || !immediateIn(op.getOperand(3), 1))
        return invalid("invalid prefetch signature or immediate");
    } else if (key.starts_with("vector_reduce_")) {
      auto vector = mlir::dyn_cast<mlir::VectorType>(first);
      if (!vector || !integer(first) || result != vector.getElementType()) return invalid("invalid vector reduction signature");
    } else if (key.ends_with("_overflow")) {
      auto record = mlir::dyn_cast_or_null<ir::RecordType>(result);
      auto flag = mlir::Type(mlir::IntegerType::get(op.getContext(), 1));
      if (auto vector = mlir::dyn_cast<mlir::VectorType>(first)) flag = mlir::VectorType::get(vector.getShape(), flag);
      if (!integer(first) || !sameArguments(2) || !record || record.isPacked() || record.getFields().size() != 2 ||
          record.getFields()[0] != first || record.getFields()[1] != flag)
        return invalid("invalid arithmetic overflow signature");
    } else if (key.starts_with("x86_") && (key.contains("_shift_right_") || key.contains("_shift_left_"))) {
      auto vector = mlir::dyn_cast<mlir::VectorType>(first);
      unsigned lanes = key.starts_with("x86_sse2_") ? 2 : key.starts_with("x86_avx2_") ? 4 : 8;
      if (!vector || vector.getNumElements() != lanes || !vector.getElementType().isInteger(64) ||
          result != first || !op.getOperand(1).getType().isInteger(32)) return invalid("invalid SSE2 shift signature");
    } else if (key == "aarch64_unsigned_widening_multiply") {
      auto input = mlir::dyn_cast<mlir::VectorType>(first);
      auto output = mlir::dyn_cast_or_null<mlir::VectorType>(result);
      if (!input || !output || !integer(first) || !integer(result) || !sameArguments(2) ||
          input.getNumElements() != output.getNumElements() ||
          input.getElementType().getIntOrFloatBitWidth() * 2 != output.getElementType().getIntOrFloatBitWidth() ||
          output.getNumElements() * output.getElementType().getIntOrFloatBitWidth() != 128)
        return invalid("invalid AArch64 widening multiply signature");
    } else if (key == "x86_ternary_logic_i32x16") {
      auto vector = mlir::dyn_cast<mlir::VectorType>(first);
      if (!vector || vector.getNumElements() != 16 || !vector.getElementType().isInteger(32) ||
          !sameArguments(3) || result != first || !op.getOperand(3).getType().isInteger(32) ||
          !immediateIn(op.getOperand(3), 255)) return invalid("invalid x86 ternary logic signature or immediate");
    } else if (key == "count_leading_zeros" || key == "count_trailing_zeros") {
      if (!integer(first) || first != result || !op.getOperand(1).getType().isInteger(1) || !immediateIn(op.getOperand(1), 1))
        return invalid("invalid zero-count signature or immediate");
    } else {
      if (!integer(first) || first != result || !sameArguments(spec->operands))
        return invalid("invalid integer intrinsic signature");
      if (key == "expect" && !immediate(op.getOperand(1))) return invalid("expect requires a constant expected value");
    }
  }
  return llvm::Error::success();
}
}
