#include <QJsonArray>
#include <QJsonObject>

#include <llvm-c/Target.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

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
#include <pp/annotations/AnnotationsHelper.h>
#include <pp/annotations/CommentAnnotation.h>
#include <pp/annotations/EntrypointAnnotation.h>
#include <pp/annotations/InstructionTypeAnnotation.h>
#include <pp/annotations/LoadRefAnnotation.h>

#include "plugins/ppCutter/core/PPCutterCore.h"
#include "Cutter.h"

Q_GLOBAL_STATIC(ppccClass, uniqueInstance)

PPCutterCore::PPCutterCore(QObject *parent) :
        QObject(parent)
{
    addAnnotationType(Annotation::Type::COMMENT, "comment");
    addAnnotationType(Annotation::Type::ENTRYPOINT, "entrypoint");
    addAnnotationType(Annotation::Type::INST_TYPE, "inst_type");
    addAnnotationType(Annotation::Type::LOAD_REF, "load_ref");
}

PPCutterCore *PPCutterCore::getInstance()
{
    return uniqueInstance;
}

PPCutterCore::~PPCutterCore()
{
}

void PPCutterCore::loadFile(std::string path)
{
    std::vector<std::shared_ptr<Annotation>> annotations;
    if (file != nullptr) {
        annotations = file->getAnnotations();
    }
    file = std::make_unique<PPBinaryFile>(path);
    file->setAnnotations(annotations);
    file->disassemble();
    ready = true;
}

std::set<const ::BasicBlock*> PPCutterCore::getBasicBlocksOfFunction(::Function& function, AddressType entrypointAddress, bool stopAtEntrypoints)
{
    std::set<const ::BasicBlock*> res;
    for (auto& frag : function) {
        //const ::Fragment* frag = *it;
        if (frag->getStartAddress() == entrypointAddress) {
            const BasicBlock *bb = llvm::dyn_cast_or_null<BasicBlock>(frag);
            if (bb == nullptr)
                continue;
            getSuccessorsRecursive(function, res, *bb, stopAtEntrypoints);
            break;
        }
    }
    return res;
}

void PPCutterCore::getSuccessorsRecursive(::Function& function, std::set<const ::BasicBlock*>& collection, const ::BasicBlock& bb, bool stopAtEntrypoints)
{
    // if it was not visited before, add all successors
    if (collection.insert(&bb).second) {
        for (auto successor = bb.succ_begin(); successor < bb.succ_end(); successor++)
        {
            if (stopAtEntrypoints && function.isEntryPoint((**successor).getStartAddress()))
                continue;
            getSuccessorsRecursive(function, collection, **successor, stopAtEntrypoints);
        }
    }
}

void PPCutterCore::addAnnotationType(Annotation::Type type, std::string str)
{
    annotationTypeToStringMap[type] = str;
    stringToAnnotationTypeMap[str] = type;
}

InstructionType PPCutterCore::parseInstructionType(const std::string iType)
{
    if (iType == "unknown")
        return UNKNOWN;
    else if (iType == "sequential")
        return SEQUENTIAL;
    else if (iType == "call.direct")
        return DIRECT_CALL;
    else if (iType == "call.indirect")
        return INDIRECT_CALL;
    else if (iType == "return")
        return RETURN;
    else if (iType == "trap")
        return TRAP;
    else if (iType == "branch.direct")
        return DIRECT_BRANCH;
    else if (iType == "branch.indirect")
        return INDIRECT_BRANCH;
    else if (iType == "branch.conditional")
        return COND_BRANCH;
    //assert(false && "missing case");
    return UNKNOWN;
}

QString PPCutterCore::toString(const InstructionType iType)
{
    switch(iType) {
        case UNKNOWN:
            return "UNKNOWN";
        case SEQUENTIAL:
            return "SEQUENTIAL";
        case DIRECT_CALL:
            return "DIRECT_CALL";
        case INDIRECT_CALL:
            return "INDIRECT_CALL";
        case RETURN:
            return "RETURN";
        case TRAP:
            return "TRAP";
        case DIRECT_BRANCH:
            return "DIRECT_BRANCH";
        case INDIRECT_BRANCH:
            return "INDIRECT_BRANCH";
        case COND_BRANCH:
            return "CONDITIONAL_BRANCH";
        default:
            return "ERROR";
    }
}

std::string PPCutterCore::toString(const Annotation::Type aType)
{
    if (annotationTypeToStringMap.count(aType))
        return annotationTypeToStringMap[aType];
    else
        return "ERROR";
}

QString PPCutterCore::annotationDataToString(const Annotation* annotation)
{
    if (const CommentAnnotation* a = llvm::dyn_cast_or_null<CommentAnnotation>(annotation)) {
        return QString::fromStdString(a->comment);
    }

    if (const EntrypointAnnotation* a = llvm::dyn_cast_or_null<EntrypointAnnotation>(annotation)) {
        return QString::fromStdString(a->name);
    }

    if (const InstructionTypeAnnotation* a = llvm::dyn_cast_or_null<InstructionTypeAnnotation>(annotation)) {
        return toString(a->instructionType);
    }

    if (const LoadRefAnnotation* a = llvm::dyn_cast_or_null<LoadRefAnnotation>(annotation)) {
        return "updateType=" + toString(a->updateType)
               + ", addrLoad=" + addrToString(a->addrLoad)
               + ", dataLoad=" + addrToString(a->dataLoad);
    }

    return "ERROR";
}

Annotation::Type PPCutterCore::annotationTypeFromString(const std::string str)
{
    if (stringToAnnotationTypeMap.count(str))
        return stringToAnnotationTypeMap[str];
    else
        return Annotation::Type::INVALID;
}

QString PPCutterCore::toString(const UpdateType updateType) {
  switch (updateType) {
    case UpdateType::CONSTANT_LOAD:
        return "CONSTANT_LOAD";
    case UpdateType::SIGNATURE_LOAD:
        return "SIGNATURE_LOAD";
    case UpdateType::CONST_INJECTION:
        return "CONST_INJECTION";
    default:
        return "INVALID";
  }
}

QString PPCutterCore::addrToString(const AddressType addr)
{
    return QString::asprintf("0x%08x", addr);
}

UpdateType PPCutterCore::updateTypeFromString(const std::string str)
{
    if (str == "CONSTANT_LOAD") {
        return UpdateType::CONSTANT_LOAD;
    } else if (str == "SIGNATURE_LOAD") {
        return UpdateType::SIGNATURE_LOAD;
    } else if (str == "CONST_INJECTION") {
        return UpdateType::CONST_INJECTION;
    }
    return UpdateType::INVALID;
}

InstructionType PPCutterCore::instTypeFromString(const std::string str)
{
    if (str == "SEQUENTIAL") {
        return InstructionType::SEQUENTIAL;
    } else if (str == "DIRECT_CALL") {
        return InstructionType::DIRECT_CALL;
    } else if (str == "INDIRECT_CALL") {
        return InstructionType::INDIRECT_CALL;
    } else if (str == "RETURN") {
        return InstructionType::RETURN;
    } else if (str == "TRAP") {
        return InstructionType::TRAP;
    } else if (str == "DIRECT_BRANCH") {
        return InstructionType::INDIRECT_BRANCH;
    } else if (str == "INDIRECT_BRANCH") {
        return InstructionType::COND_BRANCH;
    }
    return InstructionType::UNKNOWN;
}

void PPCutterCore::disassembleAll()
{
    Core()->cmd("e asm.bits=16");
    file->disassemble();
}

void PPCutterCore::calculateAll()
{
    file->calculateStates();
}

void PPCutterCore::loadProject(std::string filepath)
{
    get_logger()->set_level(spdlog::level::debug);
    file->setAnnotations(AnnotationsHelper::loadAndMatchAnnotationsFromFile(*file->state, filepath));
    file->disassemble();
    emit annotationsChanged();
}

void PPCutterCore::saveProject(std::string filepath)
{
    AnnotationsSerializer::saveAnnotationsToFile(*file->state, filepath, file->getAnnotations());
}

void PPCutterCore::registerAnnotationChange()
{
    emit annotationsChanged();
}

void PPCutterCore::registerStateChange()
{
    emit stateChanged();
}

AddressType PPCutterCore::strToAddress(QString qstr, bool* ok)
{
    if (qstr.startsWith("0x")) {
        return static_cast<AddressType>(qstr.toULong(ok, 16));
    } else {
        return static_cast<AddressType>(qstr.toULong(ok, 10));
    }
}
