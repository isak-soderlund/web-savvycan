// web-savvycan: the file-based RE tools' logic (re/filetools.h, stage 4): the Capture Bisector's split, the File
// Comparison's summaries and tree with CAN FD (bytes 8..63, bits to 511: SavvyCAN's int values[8][256] and 64-bit
// bitmap overflowed), the DBC Comparison's tree (SavvyCAN's second signal loop ran on the message index), Frame Search
// (signals past byte 8, frames too short, hex / binary patterns, 64-bit values) and FrameFileIO::loadLogData (a file's
// bytes by detected type). Under ASan/UBSan (run.sh address) overflows and leaked tree items fail the test.

#include <QtTest>
#include <QTreeWidgetItem>
#include <QTemporaryDir>
#include <functional>
#include "testframes.h"
#include "re/filetools.h"
#include "dbc/dbchandler.h"
#include "framefileio.h"
#include "framestore.h"

using namespace FileTools;

namespace {

// Text of a tree path's children: child("IDs found on both sides") etc.
QTreeWidgetItem *child(QTreeWidgetItem *parent, const QString &prefix)
{
    for (int i = 0; i < parent->childCount(); i++)
        if (parent->child(i)->text(0).startsWith(prefix)) return parent->child(i);
    return nullptr;
}
QStringList texts(QTreeWidgetItem *parent)
{
    QStringList out;
    if (parent)
        for (int i = 0; i < parent->childCount(); i++) out << parent->child(i)->text(0);
    return out;
}
struct Tree {
    QList<QTreeWidgetItem *> top;
    ~Tree() { qDeleteAll(top); }
    QTreeWidgetItem *find(const QString &prefix) const
    {
        for (QTreeWidgetItem *t : top)
            if (t->text(0).startsWith(prefix)) return t;
        return nullptr;
    }
};

CommFrame frame(quint32 id, const QByteArray &data, qint64 us = 0, int bus = 0, bool rx = true)
{
    CommFrame f = tf::makeFrame(id, data.size(), data.size() > 8, false, bus, us);
    f.setPayload(data);
    f.setReceived(rx);
    return f;
}

QByteArray bytes(int len, std::function<quint8(int)> fill)
{
    QByteArray d(len, 0);
    for (int i = 0; i < len; i++) d[i] = char(fill(i));
    return d;
}

const char *kDbcA =
    "VERSION \"\"\n\nBU_: VCU BMS INV\n\n"
    "BO_ 1960 MSG_A: 8 VCU\n"
    " SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] \"km/h\" BMS\n"
    " SG_ Gear : 16|4@1+ (1,0) [0|15] \"\" BMS\n"
    " SG_ OnlyInA : 24|8@1+ (1,0) [0|255] \"\" BMS\n\n"
    "BO_ 2016 MSG_FD: 64 BMS\n"
    " SG_ CellV : 320|16@1+ (0.001,0) [0|65.535] \"V\" VCU\n\n"
    "BO_ 1000 MSG_ONLY_A: 8 INV\n"
    " SG_ X : 0|8@1+ (1,0) [0|255] \"\" VCU\n";
const char *kDbcB =
    "VERSION \"\"\n\nBU_: VCU BMS CHG\n\n"
    "BO_ 1960 MSG_A: 8 VCU\n"
    " SG_ Speed : 0|16@1+ (0.05,0) [0|3276.75] \"km/h\" BMS\n"
    " SG_ Gear : 20|4@1+ (1,0) [0|15] \"\" BMS\n"
    " SG_ OnlyInB : 32|8@1+ (1,0) [0|255] \"\" BMS\n\n"
    "BO_ 2016 MSG_FD: 64 BMS\n"
    " SG_ CellV : 320|12@1+ (0.001,-1) [0|65.535] \"V\" VCU\n"
    " SG_ CellT : 400|8@1+ (1,-40) [-40|215] \"C\" VCU\n\n"
    "BO_ 1001 MSG_ONLY_B: 8 CHG\n"
    " SG_ Y : 0|8@1+ (1,0) [0|255] \"\" VCU\n";

} // namespace

class TestFileTools : public QObject
{
    Q_OBJECT
private slots:
    void bisect();
    void compareFd();
    void compareUniqueAndAccumulate();
    void dbcDiff();
    void searchValues();
    void searchFdAndFit();
    void searchAnyChange();
    void loadLogData();
};

void TestFileTools::bisect()
{
    QVector<CommFrame> frames;
    for (int i = 0; i < 1000; i++) frames << frame(0x100 + (i % 10), bytes(8, [](int) { return 0; }), i * 1000, i % 2);
    auto kept = [&](const BisectCriteria &c) {
        int n = 0;
        for (int i = 0; i < frames.size(); i++) n += c.keep(frames[i], i);
        return n;
    };
    BisectCriteria c;
    c.mode = BisectCriteria::FrameNumber;
    c.targetFrame = 300;
    QCOMPARE(kept(c), 300);
    c.lower = false;
    QCOMPARE(kept(c), 700);
    c.mode = BisectCriteria::Percentage;
    c.targetFrame = BisectCriteria::splitRow(1000, 2550); // 25.50 %
    QCOMPARE(c.targetFrame, qsizetype(255));
    QCOMPARE(kept(c), 745);
    c.mode = BisectCriteria::IDRange;
    c.lower = true;
    c.lowerID = 0x102;
    c.upperID = 0x104;
    QCOMPARE(kept(c), 300);
    c.lower = false;
    QCOMPARE(kept(c), 700);
    c.mode = BisectCriteria::Bus;
    c.bus = 1;
    c.lower = true;
    QCOMPARE(kept(c), 500);
}

// One ID on both sides with 64-byte FD frames: a value at byte 40 and a bit at byte 63 only in the interested file
void TestFileTools::compareFd()
{
    CompareSide a, b;
    for (int i = 0; i < 200; i++) {
        a.add(frame(0x7E8, bytes(64, [&](int j) { return j == 40 ? (i % 4 == 0 ? 0xA5 : 0x00) : j == 63 ? 0x80 : 0x11; })), nullptr);
        b.add(frame(0x7E8, bytes(64, [&](int j) { return j == 40 ? 0x00 : j == 63 ? 0x00 : 0x11; })), nullptr);
        b.add(frame(0x123, bytes(8, [](int) { return 1; })), nullptr);
        a.add(frame(0x18DAF110, bytes(12, [](int) { return 2; })), nullptr);
    }
    QCOMPARE(a.frames, quint64(400));
    QCOMPARE(a.ids.size(), 2);
    QCOMPARE(a.ids[0x7E8].dataLen, 64);
    QVERIFY(a.ids[0x7E8].values[40].test(0xA5));
    QVERIFY(a.ids[0x7E8].bitmap.test(63 * 8 + 7));
    QVERIFY(!b.ids[0x7E8].bitmap.test(63 * 8 + 7));
    QVERIFY(a.memoryBytes() < 20000);

    Tree t{buildCompareTree(a, b, "a.csv", false, nullptr)};
    QCOMPARE(t.top.size(), 3);
    QCOMPARE(texts(t.find("IDs found only in a.csv")), QStringList{Utility::formatHexNum(0x18DAF110)});
    QCOMPARE(texts(t.find("IDs found only in Side 2")), QStringList{Utility::formatHexNum(0x123)});
    QTreeWidgetItem *shared = child(t.find("IDs found on both sides"), Utility::formatHexNum(0x7E8));
    QVERIFY(shared);
    // bits: byte 40's 0xA5 = bits 0, 2, 5, 7 of byte 40, and bit 7 of byte 63, only in a
    QCOMPARE(texts(child(shared, "Bits set only in a.csv")),
             (QStringList{"320 (40:0)", "322 (40:2)", "325 (40:5)", "327 (40:7)", "511 (63:7)"}));
    QCOMPARE(texts(child(shared, "Bits set only in Side 2")), QStringList());
    QCOMPARE(texts(child(child(shared, "Byte 40"), "Values found only in a.csv")), QStringList{Utility::formatHexNum(0xA5)});
    QCOMPARE(texts(child(child(shared, "Byte 63"), "Values found only in a.csv")), QStringList{Utility::formatHexNum(0x80)});
    QCOMPARE(texts(child(child(shared, "Byte 63"), "Values found only in Side 2")), QStringList{Utility::formatHexNum(0x00)});
    QCOMPARE(texts(child(child(shared, "Byte 5"), "Values found only in a.csv")), QStringList());
    int byteItems = 0;
    for (int i = 0; i < shared->childCount(); i++) byteItems += shared->child(i)->text(0).startsWith("Byte ");
    QCOMPARE(byteItems, 64);
}

void TestFileTools::compareUniqueAndAccumulate()
{
    CompareSide a, b;
    a.add(frame(0x100, QByteArray(8, char(1))), nullptr);
    a.add(frame(0x200, QByteArray(8, char(5))), nullptr);
    b.add(frame(0x200, QByteArray(8, char(5))), nullptr);   // 0x200 the same: hidden with "unique to side 1"
    b.add(frame(0x300, QByteArray(8, char(5))), nullptr);
    Tree t{buildCompareTree(a, b, "a", true, nullptr)};
    QCOMPARE(t.top.size(), 2);
    QVERIFY(!t.find("IDs found only in Side 2"));
    QCOMPARE(texts(t.find("IDs found on both sides")), QStringList());
    QCOMPARE(texts(t.find("IDs found only in a")), QStringList{Utility::formatHexNum(0x100)});
    // side 2 takes more files: the summary grows; clear empties it
    b.add(frame(0x100, QByteArray(8, char(1))), nullptr);
    QCOMPARE(b.frames, quint64(3));
    b.clear();
    QCOMPARE(b.ids.size(), 0);
}

void TestFileTools::dbcDiff()
{
    QTemporaryDir dir;
    auto write = [&](const char *name, const char *text) {
        const QString p = dir.filePath(name);
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write(text);
        return p;
    };
    DBCFile a, b;
    QVERIFY(a.loadFile(write("a.dbc", kDbcA)));
    QVERIFY(b.loadFile(write("b.dbc", kDbcB)));
    Tree t{buildDbcDiff(&a, &b)};
    QCOMPARE(t.top.size(), 2);
    QTreeWidgetItem *nodes = t.find("Node Differences");
    QCOMPARE(texts(child(nodes, "Nodes not found in first")), QStringList{"CHG"});
    QCOMPARE(texts(child(nodes, "Nodes not found in second")), QStringList{"INV"});
    QTreeWidgetItem *msgs = t.find("Message Differences");
    QCOMPARE(texts(child(msgs, "Messages not found in first")), QStringList{"MSG_ONLY_B (0x3E9)"});
    QCOMPARE(texts(child(msgs, "Messages not found in second")), QStringList{"MSG_ONLY_A (0x3E8)"});
    QTreeWidgetItem *missing = child(msgs, "Messages with missing signals");
    QTreeWidgetItem *fromFirst = child(missing, "Missing from first");
    QTreeWidgetItem *fromSecond = child(missing, "Missing from second");
    QCOMPARE(texts(fromFirst), (QStringList{"MSG_A (0x7A8)", "MSG_FD (0x7E0)"}));
    QCOMPARE(texts(child(fromFirst, "MSG_A")), QStringList{"OnlyInB"});
    QCOMPARE(texts(child(fromFirst, "MSG_FD")), QStringList{"CellT"});
    QCOMPARE(texts(fromSecond), QStringList{"MSG_A (0x7A8)"});
    QCOMPARE(texts(child(fromSecond, "MSG_A")), QStringList{"OnlyInA"});
    QTreeWidgetItem *mod = child(msgs, "Modified Signals");
    QCOMPARE(texts(mod), (QStringList{"MSG_A (0x7A8)", "MSG_FD (0x7E0)"}));
    QCOMPARE(texts(child(mod, "MSG_A")), (QStringList{"Gear", "Speed"})); // signals by name (a QMap)
    const QStringList speed = texts(child(child(mod, "MSG_A"), "Speed"));
    QCOMPARE(speed.size(), 1);
    QVERIFY(speed[0].startsWith("Factor") && speed[0].contains("0.1") && speed[0].contains("0.05"));
    const QStringList gear = texts(child(child(mod, "MSG_A"), "Gear"));
    QVERIFY(gear.size() == 1 && gear[0].startsWith("Start Bit") && gear[0].contains("16") && gear[0].contains("20"));
    const QStringList cell = texts(child(child(mod, "MSG_FD"), "CellV"));
    QCOMPARE(cell.size(), 2); // size 16 -> 12, bias 0 -> -1
    QVERIFY(cell[0].startsWith("Size") && cell[1].startsWith("Bias"));
}

void TestFileTools::searchValues()
{
    qint64 i = 0;
    double d = 0;
    bool isInt = false;
    QVERIFY(parseSearchValue("0x1F", i, d, isInt) && isInt && i == 31);
    QVERIFY(parseSearchValue("0b0101", i, d, isInt) && isInt && i == 5);
    QVERIFY(parseSearchValue("-12", i, d, isInt) && isInt && i == -12);
    QVERIFY(parseSearchValue("12.5", i, d, isInt) && !isInt && d == 12.5);
    QVERIFY(!parseSearchValue("abc", i, d, isInt));
    QCOMPARE(signalBytesNeeded(0, 8, true), 1);
    QCOMPARE(signalBytesNeeded(56, 16, true), 9);
    QCOMPARE(signalBytesNeeded(7, 16, false), 2);    // Motorola: bits 7..0, then 15..8
    QCOMPARE(signalBytesNeeded(63, 16, false), 9);   // 63..56, then 71..64
    QCOMPARE(signalBytesNeeded(511, 16, false), 0);  // runs past bit 511
    QCOMPARE(signalBytesNeeded(504, 8, true), 64);

    // a specific value on bytes 6..7 (Intel 16 bits): too-short frames don't match 0 (SavvyCAN read them as 0)
    SearchCriteria c;
    c.valueMode = SearchCriteria::Specific;
    c.startBit = 48;
    c.numBits = 16;
    c.valueText = "0";
    FrameSearch s(c);
    QVERIFY(!s.matches(frame(0x100, QByteArray(6, 0))));
    QVERIFY(s.matches(frame(0x100, QByteArray(8, 0))));
    // scale and bias: 0x1234 * 0.5 + 10 = 2340
    c.valueText = "2340";
    c.scale = 0.5;
    c.bias = 10;
    FrameSearch s2(c);
    QVERIFY(s2.matches(frame(0x100, bytes(8, [](int j) { return j == 6 ? 0x34 : j == 7 ? 0x12 : 0; }))));
    // signed: 0xFF finds -1 typed as the pattern or as -1
    SearchCriteria n;
    n.valueMode = SearchCriteria::Specific;
    n.numBits = 8;
    n.isSigned = true;
    n.valueText = "0xFF";
    FrameSearch s3(n);
    QVERIFY(s3.matches(frame(0x1, QByteArray(8, char(0xFF)))));
    n.valueText = "-1";
    FrameSearch s4(n);
    QVERIFY(s4.matches(frame(0x1, QByteArray(8, char(0xFF)))));
    // a 64-bit pattern, exactly (no double rounding)
    SearchCriteria w;
    w.valueMode = SearchCriteria::Specific;
    w.numBits = 64;
    w.valueText = "0x0123456789ABCDEF";
    FrameSearch s5(w);
    QVERIFY(s5.matches(frame(0x2, bytes(8, [](int j) { return quint8(0xEFCDAB8967452301ULL >> (8 * j)); }))));
    QVERIFY(!s5.matches(frame(0x2, bytes(8, [](int j) { return quint8(0xEECDAB8967452301ULL >> (8 * j)); }))));
    // an invalid value
    SearchCriteria bad;
    bad.valueMode = SearchCriteria::Specific;
    bad.valueText = "x";
    QVERIFY(!FrameSearch(bad).valid());
    // ID, length, direction
    SearchCriteria f;
    f.useID = true;
    f.id = 0x7E8;
    f.useLen = true;
    f.len = 64;
    f.direction = 2;
    FrameSearch s6(f);
    QVERIFY(!s6.matches(frame(0x7E8, QByteArray(64, 0))));            // RX
    QVERIFY(s6.matches(frame(0x7E8, QByteArray(64, 0), 0, 0, false))); // TX
    QVERIFY(!s6.matches(frame(0x7E8, QByteArray(8, 0), 0, 0, false)));
    QVERIFY(!s6.matches(frame(0x7E9, QByteArray(64, 0), 0, 0, false)));
}

void TestFileTools::searchFdAndFit()
{
    // byte 40 (start bit 320) and a 12-bit Motorola value ending in byte 63
    SearchCriteria c;
    c.valueMode = SearchCriteria::Specific;
    c.startBit = 320;
    c.numBits = 8;
    c.valueText = "0b10100101";
    FrameSearch s(c);
    QVERIFY(s.matches(frame(0x7E8, bytes(64, [](int j) { return j == 40 ? 0xA5 : 0; }))));
    QVERIFY(!s.matches(frame(0x7E8, bytes(64, [](int j) { return j == 41 ? 0xA5 : 0; }))));
    QVERIFY(!s.matches(frame(0x7E8, QByteArray(8, char(0xA5)))));     // a classic frame doesn't have byte 40
    SearchCriteria m;
    m.valueMode = SearchCriteria::Specific;
    m.intel = false;
    m.startBit = 499;   // byte 62 bit 3 .. byte 63
    m.numBits = 12;
    m.valueText = "0xABC";
    FrameSearch sm(m);
    // Motorola 12 bits from bit 499: byte 62 bits 3..0 (the value's top 4 bits), then byte 63 bits 7..0
    QVERIFY(sm.matches(frame(0x7E8, bytes(64, [](int j) { return j == 62 ? 0x0A : j == 63 ? 0xBC : 0; }))));
    QVERIFY(!sm.matches(frame(0x7E8, bytes(48, [](int) { return 0; }))));
}

void TestFileTools::searchAnyChange()
{
    SearchCriteria c;
    c.valueMode = SearchCriteria::AnyChange;
    c.startBit = 400;   // byte 50
    c.numBits = 8;
    FrameSearch s(c);
    int matched = 0;
    const int values[] = {1, 1, 2, 2, 2, 3, 1};
    for (int v : values) {
        matched += s.matches(frame(0x500, bytes(64, [v](int j) { return j == 50 ? v : 0; })));
        matched += s.matches(frame(0x501, bytes(64, [](int j) { return j == 50 ? 7 : 0; }))); // only its first
    }
    QCOMPARE(matched, 4 + 1);
}

void TestFileTools::loadLogData()
{
    QByteArray csv = FrameFileIO::nativeCSVHeader();
    for (int i = 0; i < 1000; i++)
        csv += FrameFileIO::nativeCSVRow(frame(0x100 + i % 3, bytes(i % 2 ? 64 : 8, [i](int j) { return quint8(i + j); }), i * 10));
    FrameStore store(std::numeric_limits<qsizetype>::max(), FrameStore::DefaultChunkFrames);
    QVERIFY(FrameFileIO::loadLogData("x.csv", csv, &store));
    QCOMPARE(store.size(), qsizetype(1000));
    QCOMPARE(store.atIndex(999).payload().size(), 64);
    QCOMPARE(quint8(store.atIndex(999).payload()[40]), quint8(999 + 40));
    // another format (candump) through SavvyCAN's loaders from a temporary file
    const QByteArray candump = "(1.000000) can0 123#1122334455667788\n(1.001000) can0 7E8##1" + QByteArray(128, 'A') + "\n";
    FrameStore store2(std::numeric_limits<qsizetype>::max(), FrameStore::DefaultChunkFrames);
    QVERIFY(FrameFileIO::loadLogData("y.log", candump, &store2));
    QCOMPARE(store2.size(), qsizetype(2));
    QCOMPARE(store2.atIndex(1).payload().size(), 64);
    FrameStore store3(1000, 0);
    QVERIFY(!FrameFileIO::loadLogData("z.bin", QByteArray("not a log"), &store3));
}

QTEST_MAIN(TestFileTools)
#include "tst_filetools.moc"
