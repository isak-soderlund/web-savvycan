#ifndef FILETOOLS_H
#define FILETOOLS_H

/*
 * web-savvycan: the logic of the file-based reverse-engineering tools (stage 4, RESOLVE.md "Stage 4: file-based RE
 * tools"), apart from their windows so the desktop tests (tests/desktop tst_filetools) run it without the UI:
 *
 * - BisectCriteria: RE Tools > Capture Bisector's split (frame number, percentage, ID range, bus; lower/upper part).
 * - CompareSide / buildCompareTree: RE Tools > File Comparison. SavvyCAN kept both files' frames (a QVector of whole
 *   frames each) and summarised them when the second came, in `int values[8][256]` and a 64-bit bitmap per ID: CAN FD
 *   frames wrote past both (bytes 8..63), and every comparison leaked the 256 tree items per byte it didn't show.
 *   Here each file is summarised as it is loaded (per ID: the values seen per byte and the bits ever set, for all 64
 *   bytes, ~2.1 KB an ID) and its frames are dropped: memory follows the IDs, not the frames.
 * - buildDbcDiff: RE Tools > DBC Comparison's tree (SavvyCAN's loop over the second DBC's signals ran on the
 *   message index, `for (int j = 0; i < ...; i++)`: it skipped messages and read past the list).
 * - FrameSearch: RE Tools > Frame Search's matching, signals anywhere in a 64-byte frame (start bit 0..511, up to 64
 *   bits), only frames that hold the whole signal (SavvyCAN took a too-short frame's value as 0), values typed as
 *   decimal, 0x hex or 0b binary (a bit pattern, compared exactly).
 */

#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QList>
#include <array>
#include <bitset>
#include "can_structs.h"

class DBCHandler;
class DBCFile;
class DBC_SIGNAL;
class QTreeWidgetItem;

namespace FileTools {

constexpr int kMaxBytes = 64;   // CAN FD
constexpr int kMaxBits = kMaxBytes * 8;

// ---- Capture Bisector
struct BisectCriteria
{
    enum Mode { FrameNumber, Percentage, IDRange, Bus };
    Mode mode = FrameNumber;
    bool lower = true;            // keep the lower section ("Up to this frame", "Inside the ID range", "Only this bus")
    qsizetype targetFrame = 0;    // FrameNumber / Percentage: the split row (see splitRow)
    uint32_t lowerID = 0, upperID = 0;
    int bus = 0;

    // SavvyCAN's split row for a percentage slider value (0..10000 = 0..100.00 %) of `count` frames.
    static qsizetype splitRow(qsizetype count, int percentSlider)
    { return qsizetype(double(count) * (percentSlider / 10000.0)); }
    bool keep(const CommFrame &frame, qsizetype row) const;
};

// ---- File Comparison
struct FrameData
{
    uint32_t ID = 0;
    int dataLen = 0;                                  // the longest frame of the ID
    quint64 frames = 0;
    std::bitset<kMaxBits> bitmap;                     // bits set in any frame
    std::array<std::bitset<256>, kMaxBytes> values;   // per byte, the values seen
    QHash<QString, QStringList> signalInstances;      // per DBC signal, its values as text in the order first seen
    QHash<QString, QSet<QString>> signalSeen;         // the same as sets (lookups)
    QSet<QString> signalsCapped;                      // signals with more than kMaxSignalValues values (the rest not kept)
};

class CompareSide
{
public:
    static constexpr int kMaxSignalValues = 10000;   // distinct values kept per signal and ID
    void clear() { ids.clear(); frames = 0; }
    void add(const CommFrame &frame, DBCHandler *dbc);
    qsizetype memoryBytes() const;                    // roughly: the summaries (for the tests)

    QMap<uint32_t, FrameData> ids;
    quint64 frames = 0;
};

// The comparison's tree as SavvyCAN builds it: "IDs found only in <file>", "IDs found only in Side 2 - Reference
// frames" (not with uniqueInterested), "IDs found on both sides" (per ID the bits and per byte the values found on one
// side only, then the DBC signals' values). Top-level items, owned by the caller.
QList<QTreeWidgetItem *> buildCompareTree(const CompareSide &interested, const CompareSide &reference,
                                          const QString &interestedName, bool uniqueInterested, DBCHandler *dbc);

// ---- DBC Comparison: "Node Differences" and "Message Differences", SavvyCAN's tree.
QList<QTreeWidgetItem *> buildDbcDiff(DBCFile *first, DBCFile *second);

// ---- Frame Search
struct SearchCriteria
{
    bool useID = false;
    uint32_t id = 0;
    bool useLen = false;
    int len = 0;
    int direction = 0;            // 0 both, 1 RX only, 2 TX only
    enum ValueMode { NoFilter, AnyChange, Specific };
    ValueMode valueMode = NoFilter;
    int startBit = 0;             // 0..511
    int numBits = 8;              // 1..64
    bool intel = true;
    bool isSigned = false;
    double scale = 1.0, bias = 0.0;
    QString valueText;            // Specific: decimal, 0x hex or 0b binary
    DBC_SIGNAL *signal = nullptr; // a DBC signal's own decoding instead of the bits above
};

// Parses a value as typed: "0x1F" / "0b0101" / "-12" (integers, exact) or "12.5". False if it is not a number.
bool parseSearchValue(const QString &text, qint64 &integer, double &number, bool &isInteger);
// The frame's bytes the signal reads: its highest byte + 1 (Motorola bits run down a byte, then to the next byte's
// top), or 0 if the signal runs past bit 511.
int signalBytesNeeded(int startBit, int numBits, bool intel);

class FrameSearch
{
public:
    explicit FrameSearch(const SearchCriteria &c);
    bool valid() const { return mValid; }       // Specific with a value that is not a number: false
    bool matches(const CommFrame &frame);       // in frame order ("Any change" remembers the last value per ID)

private:
    SearchCriteria c;
    bool mValid = true;
    bool mExact = false;                         // compare the raw bits as an integer (no scale/bias, no DBC signal)
    qint64 mRawTarget = 0;
    double mTarget = 0;
    int mBytesNeeded = 0;
    QHash<uint32_t, qint64> mLastRaw;
    QHash<uint32_t, double> mLast;
};

} // namespace FileTools

#endif // FILETOOLS_H
