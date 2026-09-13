#include "../src/ir/ByteSwap.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <sstream>

namespace {
bool check(bool condition, llvm::StringRef message) {
  if (!condition) llvm::errs() << "byte swap test: " << message << '\n';
  return condition;
}
std::string text(const llvm::Module &module) {
  std::string result;
  llvm::raw_string_ostream output(result);
  module.print(output, nullptr);
  return result;
}
std::string replace(std::string source, const std::string &before, const std::string &after) {
  auto position = source.find(before);
  if (position == std::string::npos) llvm_unreachable("invalid byte swap fixture mutation");
  source.replace(position, before.size(), after);
  return source;
}

std::string fixture(unsigned width, bool loads = true) {
  std::string type = "i" + std::to_string(width);
  std::ostringstream source;
  source << "declare void @observe(" << type << ")\ndeclare void @escape(ptr)\n"
         << "define " << type << " @ordinary(" << type << " %input, " << type << " %other) {\nentry:\n";
  if (loads)
    source << "  %slot = alloca " << type << ", align " << width / 8 << "\n"
           << "  store " << type << " %input, ptr %slot, align " << width / 8 << "\n";
  for (unsigned byte = 0; byte < width / 8; ++byte) {
    if (loads)
      source << "  %load" << byte << " = load " << type << ", ptr %slot, align " << width / 8 << "\n";
    source << "  %mask" << byte << " = and " << type << ' '
           << (loads ? "%load" + std::to_string(byte) : "%input") << ", " << (UINT64_C(255) << (8 * byte)) << "\n";
    int shift = static_cast<int>(width - 8) - 16 * static_cast<int>(byte);
    source << "  %shift" << byte << " = " << (shift > 0 ? "shl" : "lshr") << ' ' << type
           << " %mask" << byte << ", " << (shift > 0 ? shift : -shift) << "\n";
    if (byte)
      source << "  %combined" << byte << " = or " << type << ' '
             << (byte == 1 ? "%shift0" : "%combined" + std::to_string(byte - 1))
             << ", %shift" << byte << "\n";
  }
  source << "  ret " << type << " %combined" << width / 8 - 1 << "\n}\n";
  return source.str();
}

llvm::APInt evaluate(llvm::Function &function, const llvm::APInt &input) {
  llvm::DenseMap<llvm::Value *, llvm::APInt> values;
  llvm::DenseMap<llvm::Value *, llvm::APInt> memory;
  for (auto &argument : function.args()) values[&argument] = input;
  auto value = [&](llvm::Value *operand) {
    if (auto *integer = llvm::dyn_cast<llvm::ConstantInt>(operand)) return integer->getValue();
    auto found = values.find(operand);
    if (found == values.end()) llvm_unreachable("unsupported byte swap evaluator operand");
    return found->second;
  };
  for (auto &instruction : llvm::instructions(function)) {
    if (llvm::isa<llvm::AllocaInst>(instruction) || llvm::isa<llvm::DbgInfoIntrinsic>(instruction)) continue;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      memory[store->getPointerOperand()] = value(store->getValueOperand()); continue;
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      auto found = memory.find(load->getPointerOperand());
      if (found == memory.end()) llvm_unreachable("uninitialized byte swap evaluator memory");
      values[load] = found->second; continue;
    }
    if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
      auto left = value(binary->getOperand(0)), right = value(binary->getOperand(1));
      switch (binary->getOpcode()) {
      case llvm::Instruction::And: values[binary] = left & right; break;
      case llvm::Instruction::Or: values[binary] = left | right; break;
      case llvm::Instruction::Shl: values[binary] = left.shl(right.getZExtValue()); break;
      case llvm::Instruction::LShr: values[binary] = left.lshr(right.getZExtValue()); break;
      default: llvm_unreachable("unsupported byte swap evaluator opcode");
      }
      continue;
    }
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
      if (call->getIntrinsicID() == llvm::Intrinsic::bswap)
        values[call] = value(call->getArgOperand(0)).byteSwap();
      else if (!call->getCalledFunction() || call->getCalledFunction()->getName() != "observe")
        llvm_unreachable("unsupported byte swap evaluator call");
      continue;
    }
    if (auto *result = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) return value(result->getReturnValue());
    llvm_unreachable("unsupported byte swap evaluator instruction");
  }
  llvm_unreachable("byte swap evaluator has no return");
}

std::vector<llvm::APInt> inputs(unsigned width) {
  std::vector<llvm::APInt> result{llvm::APInt(width, 0), llvm::APInt::getAllOnes(width)};
  for (unsigned bit = 0; bit < width; ++bit) result.push_back(llvm::APInt::getOneBitSet(width, bit));
  uint64_t state = UINT64_C(0x9234feaecb760154);
  for (unsigned i = 0; i < 256; ++i) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    result.emplace_back(width, state);
  }
  return result;
}

bool test(llvm::StringRef name, const std::string &source, unsigned expected, bool effect = false) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(source, diagnostic, context);
  if (!module) { diagnostic.print("byte swap fixture", llvm::errs()); return false; }
  if (llvm::verifyModule(*module, &llvm::errs())) return false;
  auto before = text(*module);
  auto *function = module->getFunction("ordinary");
  auto samples = inputs(function->getReturnType()->getIntegerBitWidth());
  std::vector<llvm::APInt> outputs;
  if (expected) for (auto input : samples) outputs.push_back(evaluate(*function, input));
  auto result = sela::detail::normalizeNativeByteSwaps(*module);
  if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false; }
  bool passed = check(*result == expected, name);
  passed &= check(!llvm::verifyModule(*module, &llvm::errs()), "normalized module verifies");
  if (!expected) passed &= check(text(*module) == before, "rejected idiom is byte-for-byte unchanged");
  else {
    for (unsigned i = 0; i < samples.size(); ++i)
      passed &= check(evaluate(*function, samples[i]) == outputs[i] && outputs[i] == samples[i].byteSwap(),
                      "all basis bits and deterministic sampled executions preserve full reversal");
    unsigned swaps = 0, effects = 0, stores = 0, allocations = 0;
    for (auto &instruction : llvm::instructions(function)) {
      stores += llvm::isa<llvm::StoreInst>(instruction);
      allocations += llvm::isa<llvm::AllocaInst>(instruction);
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        swaps += call->getIntrinsicID() == llvm::Intrinsic::bswap;
        effects += call->getCalledFunction() && call->getCalledFunction()->getName() == "observe";
      }
    }
    passed &= check(swaps == 1 && effects == (effect ? 1u : 0u), "only the proved expression changed; unrelated effects remain");
    if (source.find("alloca") != std::string::npos)
      passed &= check(stores == 1 && allocations == 1, "argument storage and initialization remain intact");
  }
  before = text(*module);
  auto repeated = sela::detail::normalizeNativeByteSwaps(*module);
  if (!repeated) { llvm::consumeError(repeated.takeError()); return false; }
  passed &= check(*repeated == 0 && text(*module) == before, "normalization is idempotent");
  return passed;
}

bool capture(llvm::StringRef path, llvm::StringRef output) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(path, diagnostic, context);
  if (!module) { diagnostic.print("byte swap capture", llvm::errs()); return false; }
  if (llvm::verifyModule(*module, &llvm::errs())) return false;
  // This name identifies a test fixture only; production normalization never
  // examines function/source names, debug records or zlib version information.
  auto *function = module->getFunction("byte_swap");
  if (!function) return check(false, "real CRC fixture contains expected test function");
  auto samples = inputs(function->getReturnType()->getIntegerBitWidth());
  std::vector<llvm::APInt> before;
  for (auto input : samples) before.push_back(evaluate(*function, input));
  auto result = sela::detail::normalizeNativeByteSwaps(*module);
  if (!result) { llvm::logAllUnhandledErrors(result.takeError(), llvm::errs()); return false; }
  bool passed = check(*result >= 1, "real CRC contains a proved byte swap") &&
                check(!llvm::verifyModule(*module, &llvm::errs()), "real normalized CRC module verifies");
  unsigned swaps = 0;
  for (auto &instruction : llvm::instructions(function))
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) swaps += call->getIntrinsicID() == llvm::Intrinsic::bswap;
  passed &= check(swaps == 1, "real CRC full-width swap becomes one native intrinsic");
  for (unsigned i = 0; i < samples.size(); ++i)
    passed &= check(evaluate(*function, samples[i]) == before[i] && before[i] == samples[i].byteSwap(), "real CRC bit reversal equivalence");
  if (!output.empty()) {
    std::error_code error;
    llvm::raw_fd_ostream stream(output, error, llvm::sys::fs::OF_Text);
    if (error) return false;
    module->print(stream, nullptr);
  }
  llvm::outs() << "Normalized " << *result << " full-width swaps in " << path << '\n';
  return passed;
}
} // namespace

int main(int argc, char **argv) {
  bool passed = true;
  for (unsigned width : {16, 32, 64}) {
    passed &= test("direct scalar argument", fixture(width, false), 1);
    passed &= test("single-store repeated argument loads", fixture(width), 1);
  }
  auto basic = fixture(32);
  passed &= test("wrong mask", replace(basic, "%load0, 255", "%load0, 254"), 0);
  passed &= test("partial masked swap", replace(basic, "%load1, 65280", "%load1, 0"), 0);
  passed &= test("wrong shift", replace(basic, "shl i32 %mask0, 24", "shl i32 %mask0, 23"), 0);
  passed &= test("shift poison flag", replace(basic, "shl i32 %mask0", "shl nuw i32 %mask0"), 0);
  passed &= test("exact poison flag", replace(basic, "lshr i32 %mask3", "lshr exact i32 %mask3"), 0);
  passed &= test("volatile load", replace(basic, "%load0 = load i32", "%load0 = load volatile i32"), 0);
  passed &= test("atomic load", replace(basic, "%load0 = load i32, ptr %slot, align 4", "%load0 = load atomic i32, ptr %slot monotonic, align 4"), 0);
  passed &= test("volatile initialization", replace(basic, "store i32 %input", "store volatile i32 %input"), 0);
  passed &= test("escaping storage", replace(basic, "  %load0 =", "  call void @escape(ptr %slot)\n  %load0 ="), 0);
  passed &= test("multiple stores", replace(basic, "  %load0 =", "  store i32 %other, ptr %slot, align 4\n  %load0 ="), 0);
  passed &= test("escaping interior value", replace(basic, "  ret i32", "  call void @observe(i32 %shift0)\n  ret i32"), 0);
  passed &= test("extra load outside graph", replace(basic, "  ret i32", "  %extra = load i32, ptr %slot, align 4\n  call void @observe(i32 %extra)\n  ret i32"), 0);
  passed &= test("two independent providers", replace(basic, "and i32 %load1,", "and i32 %other,"), 0);
  passed &= test("unrelated effect retained", replace(basic, "  %load0 =", "  call void @observe(i32 %input)\n  %load0 ="), 1, true);
  auto beforeStore = replace(basic, "  store i32 %input, ptr %slot, align 4\n", "");
  beforeStore = replace(beforeStore, "  %mask0 =", "  store i32 %input, ptr %slot, align 4\n  %mask0 =");
  passed &= test("initialization does not dominate first load", beforeStore, 0);
  auto range = replace(basic, "%load0 = load i32, ptr %slot, align 4", "%load0 = load i32, ptr %slot, align 4, !range !0");
  passed &= test("load range metadata", range + "!0 = !{i32 0, i32 100}\n", 0);
  if (argc == 2 || argc == 3) passed &= capture(argv[1], argc == 3 ? argv[2] : "");
  else if (argc != 1) return 2;
  return passed ? 0 : 1;
}
