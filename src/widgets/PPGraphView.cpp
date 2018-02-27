#include "PPGraphView.h"

#include <QPainter>
#include <QJsonObject>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QShortcut>
#include <QToolTip>
#include <QTextDocument>
#include <QFileDialog>
#include <QFile>

#include "cutter.h"
#include "utils/Colors.h"
#include "utils/Configuration.h"
#include "utils/CachedFontMetrics.h"
#include "utils/TempConfig.h"

#include <llvm-c/Target.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/FileSystem.h>
//#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>

#include <pp/ElfPatcher.h>
#include <pp/StateCalculators/AEE/ApeStateCalculator.h>
#include <pp/StateCalculators/PureSw/PureSwUpdateStateCalculator.h>
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

PPGraphView::PPGraphView(QWidget *parent, MainWindow *main)
    : GraphView(parent),
      mFontMetrics(nullptr),
      mMenu(new DisassemblyContextMenu(this)),
      main(main)
{
    highlight_token = nullptr;
    // Signals that require a refresh all
    connect(Core(), SIGNAL(refreshAll()), this, SLOT(refreshView()));
    connect(Core(), SIGNAL(commentsChanged()), this, SLOT(refreshView()));
    connect(Core(), SIGNAL(functionRenamed(const QString &, const QString &)), this, SLOT(refreshView()));
    connect(Core(), SIGNAL(flagsChanged()), this, SLOT(refreshView()));
    connect(Core(), SIGNAL(varsChanged()), this, SLOT(refreshView()));
    connect(Core(), SIGNAL(instructionChanged(RVA)), this, SLOT(refreshView()));
    connect(Core(), SIGNAL(functionsChanged()), this, SLOT(refreshView()));
    connect(Core(), SIGNAL(graphOptionsChanged()), this, SLOT(refreshView()));
    connect(Core(), SIGNAL(asmOptionsChanged()), this, SLOT(refreshView()));

    connect(Config(), SIGNAL(colorsUpdated()), this, SLOT(colorsUpdatedSlot()));
    connect(Config(), SIGNAL(fontsUpdated()), this, SLOT(fontsUpdatedSlot()));
    connect(Core(), SIGNAL(seekChanged(RVA)), this, SLOT(onSeekChanged(RVA)));

    // Space to switch to disassembly
    QShortcut *shortcut_disassembly = new QShortcut(QKeySequence(Qt::Key_Space), this);
    shortcut_disassembly->setContext(Qt::WidgetShortcut);
    connect(shortcut_disassembly, &QShortcut::activated, this, []{
        Core()->setMemoryWidgetPriority(CutterCore::MemoryWidgetType::Disassembly);
        Core()->triggerRaisePrioritizedMemoryWidget();
    });
    // ESC for previous
    QShortcut *shortcut_escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    shortcut_escape->setContext(Qt::WidgetShortcut);
    connect(shortcut_escape, SIGNAL(activated()), this, SLOT(seekPrev()));

    // Zoom shortcuts
    QShortcut *shortcut_zoom_in = new QShortcut(QKeySequence(Qt::Key_Plus), this);
    shortcut_zoom_in->setContext(Qt::WidgetShortcut);
    connect(shortcut_zoom_in, SIGNAL(activated()), this, SLOT(zoomIn()));
    QShortcut *shortcut_zoom_out = new QShortcut(QKeySequence(Qt::Key_Minus), this);
    shortcut_zoom_out->setContext(Qt::WidgetShortcut);
    connect(shortcut_zoom_out, SIGNAL(activated()), this, SLOT(zoomOut()));
    QShortcut *shortcut_zoom_reset = new QShortcut(QKeySequence(Qt::Key_Equal), this);
    shortcut_zoom_reset->setContext(Qt::WidgetShortcut);
    connect(shortcut_zoom_reset, SIGNAL(activated()), this, SLOT(zoomReset()));

    // Branch shortcuts
    QShortcut *shortcut_take_true = new QShortcut(QKeySequence(Qt::Key_T), this);
    shortcut_take_true->setContext(Qt::WidgetShortcut);
    connect(shortcut_take_true, SIGNAL(activated()), this, SLOT(takeTrue()));
    QShortcut *shortcut_take_false = new QShortcut(QKeySequence(Qt::Key_F), this);
    shortcut_take_false->setContext(Qt::WidgetShortcut);
    connect(shortcut_take_false, SIGNAL(activated()), this, SLOT(takeFalse()));

    // Navigation shortcuts
    QShortcut *shortcut_next_instr = new QShortcut(QKeySequence(Qt::Key_J), this);
    shortcut_next_instr->setContext(Qt::WidgetShortcut);
    connect(shortcut_next_instr, SIGNAL(activated()), this, SLOT(nextInstr()));
    QShortcut *shortcut_prev_instr = new QShortcut(QKeySequence(Qt::Key_K), this);
    shortcut_prev_instr->setContext(Qt::WidgetShortcut);
    connect(shortcut_prev_instr, SIGNAL(activated()), this, SLOT(prevInstr()));
    QShortcut *shortcut_next_instr_arrow = new QShortcut(QKeySequence::MoveToNextLine, this);
    shortcut_next_instr_arrow->setContext(Qt::WidgetShortcut);
    connect(shortcut_next_instr_arrow, SIGNAL(activated()), this, SLOT(nextInstr()));
    QShortcut *shortcut_prev_instr_arrow = new QShortcut(QKeySequence::MoveToPreviousLine, this);
    shortcut_prev_instr_arrow->setContext(Qt::WidgetShortcut);
    connect(shortcut_prev_instr_arrow, SIGNAL(activated()), this, SLOT(prevInstr()));
    shortcuts.append(shortcut_disassembly);
    shortcuts.append(shortcut_escape);
    shortcuts.append(shortcut_zoom_in);
    shortcuts.append(shortcut_zoom_out);
    shortcuts.append(shortcut_zoom_reset);
    shortcuts.append(shortcut_next_instr);
    shortcuts.append(shortcut_prev_instr);
    shortcuts.append(shortcut_next_instr_arrow);
    shortcuts.append(shortcut_prev_instr_arrow);

    //Export Graph menu
    mMenu->addSeparator();
    actionExportGraph.setText(tr("Export Graph"));
    mMenu->addAction(&actionExportGraph);
    connect(&actionExportGraph, SIGNAL(triggered(bool)), this, SLOT(on_actionExportGraph_triggered()));

    loadFile();
    initFont();
    colorsUpdatedSlot();
}

void PPGraphView::loadFile()
{
    auto logger = get_logger("PP-Graph");

    std::string inputFile = main->getFilename().toUtf8().constData();
    std::cout << "inputFile: " << inputFile << std::endl;
    uint64_t k0 = 0x12345678;
    uint64_t k1 = 0x8765432100000000;
    int rounds = 12;

    auto elf = llvm::make_unique<ELFIO::elfio>();
    if (!elf->load(inputFile))
    {
        std::cout << "PP: File not found" << std::endl;
        logger->error("File \"{}\" is not found or it is not an ELF file",
                      inputFile);
        exit(-1);
    }
    std::cout << "PP: File loaded" << std::endl;
    logger->debug("elf file \"{}\" successfully loaded", inputFile);

    const ELFIO::Elf_Half machine = elf->get_machine();
    std::unique_ptr<StateCalculator> stateCalc;

    if (machine == EM_ARM) {
        std::cout << "PP: identified ELF as ARM" << std::endl;

        LLVMInitializeARMTargetInfo();
        LLVMInitializeARMTargetMC();
        LLVMInitializeARMDisassembler();

        objDis = llvm::make_unique<ObjectDisassembler>(
            llvm::make_unique<Architecture::Thumb::Info>());
        state = llvm::make_unique<DisassemblerState>(objDis->getInfo());

        std::unique_ptr<StateUpdateFunction> updateFunc;
        if (false) // cli.m0_)
          updateFunc =
              llvm::make_unique<SumStateUpdateFunction<false, true>>(*state);
        else
          updateFunc =
              llvm::make_unique<CrcStateUpdateFunction<Crc32c<32>, true, true>>(
                  *state);

        stateCalc = llvm::make_unique<PureSwUpdateStateCalculator>(
            *state, std::move(updateFunc));
        stateCalc->definePreState(objDis->getInfo().sanitize(elf->get_entry()),
                                  CryptoState{4});
    } else if (machine == EM_RISCV) {
        std::cout << "PP: processing ELF as RISCV" << std::endl;

        LLVMInitializeRISCVTargetInfo();
        LLVMInitializeRISCVTargetMC();
        LLVMInitializeRISCVDisassembler();

        objDis = llvm::make_unique<ObjectDisassembler>(
            llvm::make_unique<Architecture::Riscv::Info>());
        state = llvm::make_unique<DisassemblerState>(objDis->getInfo());
        stateCalc = llvm::make_unique<ApeStateCalculator>(
            *state, llvm::make_unique<PrinceApeStateUpdateFunction>(
                        *state, k0, k1, rounds));
    }

    if (!objDis || !state || !stateCalc) {
        std::cout << "PP: Architecture of the elf file is not supported" << std::endl;
        return;
    }

    if (state->loadElf(inputFile))
        return;

    try {
        while (objDis->disassemble(*state));
        stateCalc->prepare();
        state->cleanupState();
    } catch (const Exception &e) {
        std::cout << "PP: Aborted disassembling due to exception" << e.what() << std::endl;
    }
}

PPGraphView::~PPGraphView()
{
    for(QShortcut *shortcut : shortcuts)
    {
        delete shortcut;
    }
}

void PPGraphView::refreshView()
{
    initFont();
    loadCurrentGraph();
    viewport()->update();
}

void PPGraphView::loadCurrentGraph()
{
    TempConfig tempConfig;
    tempConfig.set("scr.html", true)
            .set("scr.color", COLOR_MODE_16M)
            .set("asm.bbline", false)
            .set("asm.lines", false)
            .set("asm.fcnlines", false);
    QJsonDocument functionsDoc = Core()->cmdj("agJ");
    QJsonArray functions = functionsDoc.array();

    disassembly_blocks.clear();
    blocks.clear();

    Analysis anal;
    anal.ready = true;

    QJsonValue funcRef = functions.first();
    QJsonObject func = funcRef.toObject();
    Function f;
    f.ready = true;
    f.entry = func["offset"].toVariant().toULongLong();

    std::cout << "PP: f.entry " << f.entry << std::endl;

    ::Function *ppFunction = NULL;
    int entryPointIdx = 0;

    for (auto &&ppFunc : state->functions) {
        int epi = 0;
        for (auto &ePoint : ppFunc.getEntryPoints()) {
            std::cout << "PP: function <" << ePoint.name << "> @ " << ePoint.address << std::endl;
            if (f.entry == ePoint.address) {
                ppFunction = &ppFunc;
                entryPointIdx = epi;
                std::cout << "PP: found!!" << std::endl;
            }
            epi++;
        }
    }

    if (!ppFunction) {
        return;
    }
    for (auto it = ppFunction->begin(); it != ppFunction->end(); ++it)
    {
        const ::Fragment* frag = *it;
        std::cout << "PP: fragment " << frag->getStartAddress() << std::endl;
        const BasicBlock *bb = llvm::dyn_cast_or_null<BasicBlock>(frag);
        std::cout << "PP: BasicBlock " << frag->getStartAddress() << std::endl;

        if (!bb)
            continue;


        // get address of first instruction (= address of block)
        RVA block_entry = (bb->inst_begin())->address;


        DisassemblyBlock db;
        GraphBlock gb;
        gb.entry = block_entry;
        db.entry = block_entry;
        db.true_path = RVA_INVALID;
        db.false_path = RVA_INVALID;

        // mark block if it is the entry of the function
        if (ppFunction->getEntryPoints()[entryPointIdx].address == block_entry)
        {
            RichTextPainter::List rtpl;
            RichTextPainter::CustomRichText_t title;
            title.highlight = true;
            title.flags = RichTextPainter::FlagColor;
            std::string titles = "Entry Point: " + ppFunction->getEntryPoints()[entryPointIdx].name;
            title.text = QString::fromUtf8(titles.c_str());
            title.textColor = ConfigColor("fname");
            rtpl.push_back(title);
            db.header_text = rtpl;
        }

        for (auto sit = bb->succ_begin(); sit != bb->succ_end(); ++sit) {
            const BasicBlock *succ = *sit;
            // get address of first instruction (= address of block)
            RVA addr = (succ->inst_begin())->address;
            gb.exits.push_back(addr);
        }

        for (auto dii = bb->inst_begin(); dii != bb->inst_end(); ++dii) {
            const DecodedInstruction di = *dii;
            std::cout << "PP: instr " << di.type << std::endl;

            Instr i;
            i.addr = di.address;
            // Skip last byte, otherwise it will overlap with next instruction
            i.size = di.instruction.size() - 1;

            QString color = "#000000";
            if (di.isTerminator(*state) == CERTAIN) {
                color = "#2080d0";
            }
            std::string asmString = objDis->getInfo().printInstrunction(di.instruction);
            QString asmQString = QString::fromUtf8(asmString.c_str());
            QString disas = QString("<font color='#000000'>%1</font>&nbsp;&nbsp;<font color='%3'>%2")
              .arg(di.address, 8, 16, QChar('0'))
              .arg(asmQString)
              .arg(color);

            QTextDocument textDoc;
            textDoc.setHtml(disas);

            RichTextPainter::List richText = RichTextPainter::fromTextDocument(textDoc);

            bool cropped;
            int blockLength = Config()->getGraphBlockMaxChars() + Core()->getConfigb("asm.bytes") * 24 + Core()->getConfigb("asm.emu") * 10;
            i.text = Text(RichTextPainter::cropped(richText, blockLength, "...", &cropped));
            if(cropped)
                i.fullText = richText;
            else
                i.fullText = Text();
            db.instrs.push_back(i);
        }


        disassembly_blocks[db.entry] = db;
        prepareGraphNode(gb);
        f.blocks.push_back(db);

        addBlock(gb);
    }

    QString windowTitle = tr("PP-Graph");
    QString funcName = func["name"].toString().trimmed();
    if (ppFunction != NULL)
    {
        std::string ppFunctionName = ppFunction->getEntryPoints()[entryPointIdx].name;
        QString qname = QString::fromUtf8(ppFunctionName.c_str());
        windowTitle += " (" + qname + ")";
    }
    parentWidget()->setWindowTitle(windowTitle);

    RVA entry = func["offset"].toVariant().toULongLong();

    setEntry(entry);
    /*for (QJsonValueRef blockRef : func["blocks"].toArray()) {
        QJsonObject block = blockRef.toObject();
        RVA block_entry = block["offset"].toVariant().toULongLong();
        RVA block_size = block["size"].toVariant().toULongLong();
        RVA block_fail = block["fail"].toVariant().toULongLong();
        RVA block_jump = block["jump"].toVariant().toULongLong();

        DisassemblyBlock db;
        GraphBlock gb;
        gb.entry = block_entry;
        db.entry = block_entry;
        db.true_path = RVA_INVALID;
        db.false_path = RVA_INVALID;
        if(block_fail)
        {
            db.false_path = block_fail;
            gb.exits.push_back(block_fail);
        }
        if(block_jump)
        {
            if(block_fail)
            {
                db.true_path = block_jump;
            }
            gb.exits.push_back(block_jump);
        }
        QJsonArray opArray = block["ops"].toArray();
        for (int opIndex=0; opIndex<opArray.size(); opIndex++)
        {
            QJsonObject op = opArray[opIndex].toObject();
            Instr i;
            i.addr = op["offset"].toVariant().toULongLong();

            if (opIndex < opArray.size() - 1)
            {
                // get instruction size from distance to next instruction ...
                RVA nextOffset = opArray[opIndex+1].toObject()["offset"].toVariant().toULongLong();
                i.size = nextOffset - i.addr;
            }
            else
            {
                // or to the end of the block.
                i.size = (block_entry + block_size) - i.addr;
            }

            // Skip last byte, otherwise it will overlap with next instruction
            i.size -= 1;

            QString disas;
            disas = op["text"].toString();

			QTextDocument textDoc;
			textDoc.setHtml(disas);

            RichTextPainter::List richText = RichTextPainter::fromTextDocument(textDoc);
            //Colors::colorizeAssembly(richText, textDoc.toPlainText(), 0);

            bool cropped;
            int blockLength = Config()->getGraphBlockMaxChars() + Core()->getConfigb("asm.bytes") * 24 + Core()->getConfigb("asm.emu") * 10;
            i.text = Text(RichTextPainter::cropped(richText, blockLength, "...", &cropped));
            if(cropped)
                i.fullText = richText;
            else
                i.fullText = Text();
            db.instrs.push_back(i);
        }
        disassembly_blocks[db.entry] = db;
        prepareGraphNode(gb);
        f.blocks.push_back(db);

        addBlock(gb);
    }*/

    anal.functions[f.entry] = f;
    anal.status = "Ready.";
    anal.entry = f.entry;

    if(func["blocks"].toArray().size() > 0)
    {
        computeGraph(entry);
        viewport()->update();

        if(first_draw)
        {
            showBlock(blocks[entry]);
            first_draw = false;
        }
    }
}

void PPGraphView::prepareGraphNode(GraphBlock &block)
{
    DisassemblyBlock &db = disassembly_blocks[block.entry];
    int width = 0;
    int height = 0;
    for(auto & line : db.header_text.lines)
    {
        int lw = 0;
        for(auto & part : line)
            lw += mFontMetrics->width(part.text);
        if(lw > width)
            width = lw;
        height += 1;
    }
    for(Instr & instr : db.instrs)
    {
        for(auto & line : instr.text.lines)
        {
            int lw = 0;
            for(auto & part : line)
                lw += mFontMetrics->width(part.text);
            if(lw > width)
                width = lw;
            height += 1;
        }
    }
    int extra = 4 * charWidth + 4;
    block.width = width + extra + charWidth;
    block.height = (height * charHeight) + extra;
}


void PPGraphView::initFont()
{
    setFont(Config()->getFont());
    QFontMetricsF metrics(font());
    baseline = int(metrics.ascent());
    charWidth = metrics.width('X');
    charHeight = metrics.height();
    charOffset = 0;
    if(mFontMetrics)
        delete mFontMetrics;
    mFontMetrics = new CachedFontMetrics(this, font());
}

void PPGraphView::drawBlock(QPainter & p, GraphView::GraphBlock &block)
{
    p.setPen(Qt::black);
    p.setBrush(Qt::gray);
    p.drawRect(block.x, block.y, block.width, block.height);


    // Render node
    DisassemblyBlock &db = disassembly_blocks[block.entry];
    bool block_selected = false;
    RVA selected_instruction = RVA_INVALID;

    // Figure out if the current block is selected
    for(const Instr & instr : db.instrs)
    {
        RVA addr = Core()->getOffset();
        if((instr.addr <= addr) && (addr <= instr.addr+instr.size))
        {
            block_selected = true;
            selected_instruction = instr.addr;
        }
        // TODO: L219
    }

    p.setPen(QColor(0, 0, 0, 0));
    if(db.terminal)
    {
        p.setBrush(retShadowColor);
    } else if(db.indirectcall) {
        p.setBrush(indirectcallShadowColor);
    } else {
        p.setBrush(QColor(0, 0, 0, 128));
    }

    p.drawRect(block.x + 4, block.y + 4,
               block.width + 4, block.height + 4);
    p.setPen(graphNodeColor);

    if(block_selected)
    {
        p.setBrush(disassemblySelectedBackgroundColor);
    } else {
        p.setBrush(disassemblyBackgroundColor);
    }

    p.drawRect(block.x, block.y,
               block.width, block.height);

    // Draw different background for selected instruction
    if(selected_instruction != RVA_INVALID)
    {
        int y = block.y + (2 * charWidth) + (db.header_text.lines.size() * charHeight);
        for(Instr & instr : db.instrs)
        {
            auto selected = instr.addr == selected_instruction;
            //auto traceCount = dbgfunctions->GetTraceRecordHitCount(instr.addr);
            auto traceCount = 0;
            if(selected && traceCount)
            {
                p.fillRect(QRect(block.x + charWidth, y, block.width - (10 + 2 * charWidth),
                                 int(instr.text.lines.size()) * charHeight), disassemblyTracedSelectionColor);
            }
            else if(selected)
            {
                p.fillRect(QRect(block.x + charWidth, y, block.width - (10 + 2 * charWidth),
                                 int(instr.text.lines.size()) * charHeight), disassemblySelectionColor);
            }
            else if(traceCount)
            {
                // Color depending on how often a sequence of code is executed
                int exponent = 1;
                while(traceCount >>= 1) //log2(traceCount)
                    exponent++;
                int colorDiff = (exponent * exponent) / 2;

                // If the user has a light trace background color, substract
                if(disassemblyTracedColor.blue() > 160)
                    colorDiff *= -1;

                p.fillRect(QRect(block.x + charWidth, y, block.width - (10 + 2 * charWidth), int(instr.text.lines.size()) * charHeight),
                           QColor(disassemblyTracedColor.red(),
                                  disassemblyTracedColor.green(),
                                  std::max(0, std::min(256, disassemblyTracedColor.blue() + colorDiff))));
            }
            y += int(instr.text.lines.size()) * charHeight;
        }
    }


    // Render node text
    auto x = block.x + (2 * charWidth);
    int y = block.y + (2 * charWidth);
    for(auto & line : db.header_text.lines)
    {
        RichTextPainter::paintRichText(&p, x, y, block.width, charHeight, 0, line, mFontMetrics);
        y += charHeight;
    }
    for(Instr & instr : db.instrs)
    {
        for(auto & line : instr.text.lines)
        {
            int rectSize = qRound(charWidth);
            if(rectSize % 2)
            {
                rectSize++;
            }
            // Assume charWidth <= charHeight
            QRectF bpRect(x - rectSize / 3.0, y + (charHeight - rectSize) / 2.0, rectSize, rectSize);

            // TODO: Breakpoint/Cip stuff

            RichTextPainter::paintRichText(&p, x + charWidth, y, block.width - charWidth, charHeight, 0, line, mFontMetrics);
            y += charHeight;

        }
    }
}

GraphView::EdgeConfiguration PPGraphView::edgeConfiguration(GraphView::GraphBlock &from, GraphView::GraphBlock *to)
{
    EdgeConfiguration ec;
    DisassemblyBlock &db = disassembly_blocks[from.entry];
    if(to->entry == db.true_path)
    {
        ec.color = brtrueColor;
    }
    else if(to->entry == db.false_path)
    {
        ec.color = brfalseColor;
    }
    else
    {
        ec.color = jmpColor;
    }
    ec.start_arrow = false;
    ec.end_arrow = true;
    return ec;
}

RVA PPGraphView::getAddrForMouseEvent(GraphBlock &block, QPoint *point)
{
    DisassemblyBlock &db = disassembly_blocks[block.entry];

    // Remove header and margin
    int off_y = (2 * charWidth) + (db.header_text.lines.size() * charHeight);
    // Get mouse coordinate over the actual text
    int text_point_y = point->y() - off_y;
    int mouse_row = text_point_y / charHeight;

    int cur_row = db.header_text.lines.size();
    if (mouse_row < cur_row)
    {
        return db.entry;
    }

    Instr *instr = getInstrForMouseEvent(block, point);
    if(instr)
    {
        return instr->addr;
    }

    return RVA_INVALID;
}


PPGraphView::Instr *PPGraphView::getInstrForMouseEvent(GraphView::GraphBlock &block, QPoint *point)
{
    DisassemblyBlock &db = disassembly_blocks[block.entry];

    // Remove header and margin
    int off_y = (2 * charWidth) + (db.header_text.lines.size() * charHeight);
    // Get mouse coordinate over the actual text
    int text_point_y = point->y() - off_y;
    int mouse_row = text_point_y / charHeight;

    int cur_row = db.header_text.lines.size();

    for(Instr & instr : db.instrs)
    {
        if(mouse_row < cur_row + (int)instr.text.lines.size())
        {
            return &instr;
        }
        cur_row += instr.text.lines.size();
    }

    return nullptr;
}

// Public Slots

void PPGraphView::colorsUpdatedSlot()
{
    disassemblyBackgroundColor = ConfigColor("gui.alt_background");
    disassemblySelectedBackgroundColor = ConfigColor("gui.background");
    mDisabledBreakpointColor = disassemblyBackgroundColor;
    graphNodeColor = ConfigColor("gui.border");
    backgroundColor = ConfigColor("gui.background");
    disassemblySelectionColor = ConfigColor("highlight");

    jmpColor = QColor(0, 0, 0); //ConfigColor("graph.trufae");
    brtrueColor = ConfigColor("graph.true");
    brfalseColor = ConfigColor("graph.false");

    mCommentColor = ConfigColor("comment");
    initFont();
    refreshView();
}

void PPGraphView::fontsUpdatedSlot()
{
    initFont();
    refreshView();
}

PPGraphView::DisassemblyBlock *PPGraphView::blockForAddress(RVA addr)
{
    for(auto & blockIt : disassembly_blocks)
    {
        DisassemblyBlock &db = blockIt.second;
        for(Instr i : db.instrs)
        {
            if((i.addr <= addr) && (addr <= i.addr + i.size))
            {
                return &db;
            }
        }
    }
    return nullptr;
}

void PPGraphView::onSeekChanged(RVA addr)
{
    mMenu->setOffset(addr);
    // If this seek was NOT done by us...
    if(!sent_seek)
    {
        DisassemblyBlock *db = blockForAddress(addr);
        if(db)
        {
            // This is a local address! We animated to it.
            transition_dont_seek = true;
            showBlock(&blocks[db->entry], true);
            return;
        } else {
            refreshView();
            DisassemblyBlock *db = blockForAddress(addr);
            if(db)
            {
                // This is a local address! We animated to it.
                transition_dont_seek = true;
                showBlock(&blocks[db->entry], false);
                return;
            }
        }
    }
    sent_seek = false;
}

void PPGraphView::zoomIn()
{
    current_scale += 0.1;
    auto areaSize = viewport()->size();
    adjustSize(areaSize.width(), areaSize.height());
    viewport()->update();
}

void PPGraphView::zoomOut()
{
    current_scale -= 0.1;
    current_scale = std::max(current_scale, 0.3);
    auto areaSize = viewport()->size();
    adjustSize(areaSize.width(), areaSize.height());
    viewport()->update();
}

void PPGraphView::zoomReset()
{
    current_scale = 1.0;
    auto areaSize = viewport()->size();
    adjustSize(areaSize.width(), areaSize.height());
    viewport()->update();
}

void PPGraphView::takeTrue()
{
    DisassemblyBlock *db = blockForAddress(Core()->getOffset());
    if(db->true_path != RVA_INVALID)
    {
        Core()->seek(db->true_path);
    }
    else if(blocks[db->entry].exits.size())
    {
        Core()->seek(blocks[db->entry].exits[0]);
    }
}

void PPGraphView::takeFalse()
{
    DisassemblyBlock *db = blockForAddress(Core()->getOffset());
    if(db->false_path != RVA_INVALID)
    {
        Core()->seek(db->false_path);
    }
    else if(blocks[db->entry].exits.size())
    {
        Core()->seek(blocks[db->entry].exits[0]);
    }
}

void PPGraphView::seekInstruction(bool previous_instr)
{
    RVA addr = Core()->getOffset();
    DisassemblyBlock *db = blockForAddress(addr);
    if(!db)
    {
        return;
    }

    for(size_t i=0; i < db->instrs.size(); i++)
    {
        Instr &instr = db->instrs[i];
        if(!((instr.addr <= addr) && (addr <= instr.addr + instr.size)))
        {
            continue;
        }

        // Found the instructon. Check if a next one exists
        if(!previous_instr && (i < db->instrs.size()-1))
        {
            seek(db->instrs[i+1].addr, true);
        }
        else if(previous_instr && (i > 0))
        {
            seek(db->instrs[i-1].addr);
        }
    }
}

void PPGraphView::nextInstr()
{
    seekInstruction(false);
}

void PPGraphView::prevInstr()
{
    seekInstruction(true);
}

void PPGraphView::seek(RVA addr, bool update_viewport)
{
    sent_seek = true;
    Core()->seek(addr);
    if(update_viewport)
    {
        viewport()->update();
    }
}

void PPGraphView::seekPrev()
{
    Core()->seekPrev();
}

void PPGraphView::blockClicked(GraphView::GraphBlock &block, QMouseEvent *event, QPoint pos)
{
    RVA instr = getAddrForMouseEvent(block, &pos);
    if(instr == RVA_INVALID)
    {
        return;
    }

    seek(instr, true);

    if(event->button() == Qt::RightButton)
    {
        mMenu->setOffset(instr);
        mMenu->exec(event->globalPos());
    }
}

void PPGraphView::blockDoubleClicked(GraphView::GraphBlock &block, QMouseEvent *event, QPoint pos)
{
    Q_UNUSED(event);
    RVA instr = getAddrForMouseEvent(block, &pos);
    if(instr == RVA_INVALID)
    {
        return;
    }
    QList<XrefDescription> refs = Core()->getXRefs(instr, false, false);
    if (refs.length()) {
        sent_seek = false;
        Core()->seek(refs.at(0).to);
    }
    if (refs.length() > 1) {
        qWarning() << "Too many references here. Weird behaviour expected.";
    }
}

void PPGraphView::blockHelpEvent(GraphView::GraphBlock &block, QHelpEvent *event, QPoint pos)
{
    Instr *instr = getInstrForMouseEvent(block, &pos);
    if(!instr || instr->fullText.lines.empty())
    {
        QToolTip::hideText();
        event->ignore();
        return;
    }

    QToolTip::showText(event->globalPos(), instr->fullText.ToQString());
}

bool PPGraphView::helpEvent(QHelpEvent *event)
{
    if(!GraphView::helpEvent(event))
    {
        QToolTip::hideText();
        event->ignore();
    }

    return true;
}

void PPGraphView::blockTransitionedTo(GraphView::GraphBlock *to)
{
    if(transition_dont_seek)
    {
        transition_dont_seek = false;
        return;
    }
    seek(to->entry);
}

void PPGraphView::on_actionExportGraph_triggered()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        tr("Export Graph"), "", tr("Dot file (*.dot)"));
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)){
        qWarning() << "Can't open file";
        return;
    }
    QTextStream fileOut(&file);
    fileOut << Core()->cmd("ag -");
}
