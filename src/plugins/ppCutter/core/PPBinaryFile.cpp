#include "PPBinaryFile.h"

#include <llvm-c/Target.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/FileSystem.h>
//#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>

#include <sstream>

#include <pp/ElfPatcher.h>
#include <pp/StateUpdateFunctions/crc/CrcStateUpdateFunction.hpp>
#include <pp/StateUpdateFunctions/prince_ape/PrinceApeStateUpdateFunction.hpp>
#include <pp/StateUpdateFunctions/sum/SumStateUpdateFunction.hpp>
#include <pp/architecture/riscv/info.h>
#include <pp/architecture/riscv/replace_instructions.h>
#include <pp/architecture/thumbv7m/info.h>
#include <pp/basicblock.h>
#include <pp/config.h>
#include <pp/disassemblerstate.h>
#include <pp/exception.h>
#include <pp/logger.h>
#include <pp/types.h>
#include <pp/function.h>
#include <pp/config.h>
#include <pp/annotations/Annotation.h>
#include <pp/annotations/CommentAnnotation.h>
#include <pp/annotations/EntrypointAnnotation.h>
#include <pp/annotations/InstructionTypeAnnotation.h>
#include <pp/annotations/LoadRefAnnotation.h>
#include <pp/annotations/AnnotationsHelper.h>
#include <pp/annotations/AnnotationsSerializer.h>

#include "PPCutterCore.h"

PPBinaryFile::PPBinaryFile(std::string inputFile)
{
  std::cout << "inputFile: " << inputFile << std::endl;
  uint64_t k0 = 0x12345678;
  uint64_t k1 = 0x8765432100000000;
  int rounds = 12;

  auto elf = std::make_unique<ELFIO::elfio>();
  if (!elf->load(inputFile))
  {
    std::cout << "PP: File not found" << std::endl;
    std::cerr << "File '" << inputFile << "' not found or it is not an ELF file";
    exit(-1);
  }
  std::cout << "PP: File loaded" << std::endl;

  machine = elf->get_machine();

  std::cout << "PP: machine (" << machine << ")" << std::endl;

#ifdef ARM_TARGET_ENABLED
  std::cout << "PP: checking for ARM (" << EM_ARM << ")" << std::endl;
    if (machine == EM_ARM) {
      std::cout << "PP: identified ELF as ARM" << std::endl;

      LLVMInitializeARMTargetInfo();
      LLVMInitializeARMTargetMC();
      LLVMInitializeARMDisassembler();

      objDis = std::make_unique<ObjectDisassembler>(
          std::make_unique<Architecture::Thumb::Info>());
      state = std::make_unique<DisassemblerState>(objDis->getInfo());

      std::unique_ptr<StateUpdateFunction> updateFunc;
      if (false) // (cli.m0_)
        updateFunc =
            std::make_unique<SumStateUpdateFunction<false, true>>(*state);
      else
        updateFunc =
            std::make_unique<CrcStateUpdateFunction<Crc32c<32>, true, true>>(
                *state);

      stateCalc = std::make_unique<PureSwUpdateStateCalculator>(
          *state, std::move(updateFunc));
      stateCalc->definePreState(objDis->getInfo().sanitize(elf->get_entry()),
                                CryptoState{4});
    }
#endif // ARM_TARGET_ENABLED
#ifdef RISCV_TARGET_ENABLED
  std::cout << "PP: checking for RISCV (" << EM_RISCV << ")" << std::endl;
    if (machine == EM_RISCV) {
      std::cout << "PP: identified ELF as RISCV" << std::endl;

      LLVMInitializeRISCVTargetInfo();
      LLVMInitializeRISCVTargetMC();
      LLVMInitializeRISCVDisassembler();

      bool rv32 = elf->get_class() == ELFCLASS32;
      objDis = std::make_unique<ObjectDisassembler>(
          std::make_unique<Architecture::Riscv::Info>(rv32));

      state = std::make_unique<DisassemblerState>(objDis->getInfo());
      stateCalc = std::make_unique<ApeStateCalculator>(
          *state, std::make_unique<PrinceApeStateUpdateFunction>(
                      *state, k0, k1, rounds));
    }
#endif // RISCV_TARGET_ENABLED

  if (!objDis || !state || !stateCalc) {
    std::cout << "PP: Architecture of the elf file is not supported" << std::endl;
    return;
  }

  if (state->loadElf(inputFile))
    return;
}

void PPBinaryFile::createIndex()
{
  if (!objDis || !state || !stateCalc) {
    std::cout << "PP: Architecture of the elf file is not supported" << std::endl;
    return;
  }
  AnnotationsHelper::prepareAnnotations(*state, annotations);
}

void PPBinaryFile::disassemble()
{
  if (!objDis || !state || !stateCalc) {
    std::cout << "PP: Architecture of the elf file is not supported" << std::endl;
    return;
  }
  AnnotationsHelper::prepareAnnotations(*state, annotations);

  int num_rounds = 0;
  try {
    while (objDis->disassemble(*state)) num_rounds++;
    std::cout << "rounds: " << num_rounds << std::endl;
    std::cout << "functions: " << state->functions.size() << std::endl;
  } catch (const Exception &e) {
    std::cout << "PP: Aborted disassembling due to exception: " << e.what() << std::endl;
  }

  try {
    stateCalc->prepare();
    state->cleanupState();
  } catch (const Exception &e) {
    std::cout << "PP: could not prepare state due to: " << e.what() << std::endl;
  }

  buildFunctionCache();
  PPCore()->registerStateChange();
}

bool PPBinaryFile::calculateStates()
{
  std::vector<StateFixup> fixups;
  try {
    fixups = stateCalc->calculate();
  } catch (const Exception &e) {
    std::cout << "Aborted calculation due to: " << e.what() << std::endl;
    return false;
  }
  PPCore()->registerStateChange();
  return true;
}

void PPBinaryFile::buildFunctionCache()
{
  entrypoint_ranges.clear();
  for (auto &&function : state->functions) {
    for (auto &&entrypoint : function.getEntryPoints()) {
      AddressType end = 0;
      for (auto &&bb : PPCore()->getBasicBlocksOfFunction(function, entrypoint.address, true)) {
        if (end < bb->getEndAddress())
          end = bb->getEndAddress();
      }
      entrypoint_ranges.push_back({entrypoint.address, end, entrypoint.name});
    }
  }
}

PPBinaryFile::~PPBinaryFile()
{
}

::Function* PPBinaryFile::getFunctionAt(AddressType addr) const
{
  std::string name;
  for (const EntryPointRange& epr : entrypoint_ranges) {
    if (addr >= epr.start && addr <= epr.end) {
      name = epr.functionName;
      break;
    }
  }
  for (auto &&function : state->functions) {
//    if (function.getJoinedName() == name)
//      return &function;

    for (::Function::EntryPoint& ep : function.getEntryPoints()) {
      if (ep.name == name) {
        return &function;
      }
    }
  }
  return nullptr;
}

::Function::EntryPoint& PPBinaryFile::getEntrypointAt(AddressType addr) const
{
  std::string name;
  for (const EntryPointRange& epr : entrypoint_ranges) {
    if (addr >= epr.start && addr <= epr.end) {
      name = epr.functionName;
      break;
    }
  }
  for (auto &&function : state->functions) {
    for (::Function::EntryPoint& ep : function.getEntryPoints()) {
      if (ep.name == name) {
        return ep;
      }
    }
  }
  assert(false);
}

AddressType PPBinaryFile::getStartAddressOfFunction(const ::Function& function) const
{
  //return (*function.begin())->getStartAddress();

  AddressType start = 0;

  for (auto& fragment : function) {
    if (start > fragment->getEndAddress())
      start = fragment->getStartAddress();
  }

  return start;
}

AddressType PPBinaryFile::getEndAddressOfFunction(const ::Function& function) const
{
  AddressType end = 0;

  for (auto& fragment : function) {
    if (end < fragment->getEndAddress())
      end = fragment->getEndAddress();
  }

  return end;
}

std::set<std::shared_ptr<Annotation>> PPBinaryFile::getAnnotationsAt(AddressType addr)
{
  return state->annotations_by_address[addr];
}

std::shared_ptr<Annotation> PPBinaryFile::createAnnotation(Annotation::Type type, AddressType anchorAddress)
{
  std::shared_ptr<Annotation> ret;

  switch (type) {
    case Annotation::Type::COMMENT:
      ret = std::make_shared<CommentAnnotation>(anchorAddress);
      break;
    case Annotation::Type::ENTRYPOINT:
      ret = std::make_shared<EntrypointAnnotation>(anchorAddress);
      break;
    case Annotation::Type::INST_TYPE:
      ret = std::make_shared<InstructionTypeAnnotation>(anchorAddress);
      break;
    case Annotation::Type::LOAD_REF:
      ret = std::make_shared<LoadRefAnnotation>(anchorAddress);
      break;
    default:
      std::cout << "Annotation::Type not implemented: no. " << static_cast<int>(type) << std::endl;
      assert(false);
  }

  annotations.push_back(ret);

  AnnotationsHelper::prepareAnnotations(*state, annotations);

  PPCore()->registerAnnotationChange();

  return ret;
}

void PPBinaryFile::deleteAnnotation(std::shared_ptr<Annotation> annotation)
{
  annotations.erase(std::remove(annotations.begin(), annotations.end(), annotation), annotations.end());
  PPCore()->registerAnnotationChange();
}

std::set<AddressType> PPBinaryFile::getAssociatedAddresses(AddressType addr)
{
  std::set<AddressType> res;
  for (auto& annotation : state->annotations_by_address[addr]) {
    if (const LoadRefAnnotation* a = llvm::dyn_cast<LoadRefAnnotation>(annotation.get())) {
      res.insert(a->address);
      res.insert(a->addrLoad);
      res.insert(a->dataLoad);
    }
  }
  return res;
}

std::string PPBinaryFile::getStates(AddressType addr)
{
  std::stringstream res;
  if (stateCalc->preStates()->count(addr))
    res << stateCalc->preStates()->at(addr);
  else
    res << "           ";
  res << " -> ";
  if (stateCalc->postStates()->count(addr))
    res << stateCalc->postStates()->at(addr);
  else
    res << "           ";
  return res.str();
}
