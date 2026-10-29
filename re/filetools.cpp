#include "filetools.h"
#include "utility.h"
#include "dbc/dbchandler.h"
#include "dbc/dbc_classes.h"
#include <QTreeWidgetItem>
#include <limits>

namespace FileTools {

// ---------------------------------------------------------------------------------------------------- Bisector
bool BisectCriteria::keep(const CommFrame &frame, qsizetype row) const
{
    switch (mode) {
    case FrameNumber:
    case Percentage:
        return lower ? row < targetFrame : row >= targetFrame;
    case IDRange: {
        const bool inside = frame.frameId() >= lowerID && frame.frameId() <= upperID;
        return lower ? inside : !inside;
    }
    case Bus:
        return lower ? frame.getBus() == bus : frame.getBus() != bus;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------- File Comparison
void CompareSide::add(const CommFrame &frame, DBCHandler *dbc)
{
    frames++;
    const uint32_t id = frame.frameId();
    auto it = ids.find(id);
    if (it == ids.end()) {
        it = ids.insert(id, FrameData());
        it->ID = id;
    }
    FrameData &d = *it;
    d.frames++;
    const QByteArray payload = frame.payload();
    const int len = int(qMin<qsizetype>(payload.size(), kMaxBytes));
    const auto *data = reinterpret_cast<const unsigned char *>(payload.constData());
    if (len > d.dataLen) d.dataLen = len;
    for (int y = 0; y < len; y++) {
        d.values[size_t(y)].set(data[y]);
        for (int b = 0; b < 8; b++)
            if (data[y] & (1 << b)) d.bitmap.set(size_t(y * 8 + b));
    }
    if (!dbc) return;
    DBC_MESSAGE *msg = dbc->findMessage(id);
    if (!msg) return;
    const QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();
    for (DBC_SIGNAL *sig : sigs) {
        if (!sig || !sig->isSignalInMessage(frame)) continue;
        QString sigVal;
        if (!sig->processAsText(frame, sigVal, false)) continue;
        QSet<QString> &seen = d.signalSeen[sig->name];
        if (seen.contains(sigVal)) continue;
        if (seen.size() >= kMaxSignalValues) {
            d.signalsCapped.insert(sig->name);
            continue;
        }
        seen.insert(sigVal);
        d.signalInstances[sig->name].append(sigVal);
    }
}

qsizetype CompareSide::memoryBytes() const
{
    qsizetype bytes = 0;
    for (const FrameData &d : ids) {
        bytes += qsizetype(sizeof(FrameData)) + 32;
        for (const QStringList &l : d.signalInstances)
            for (const QString &s : l) bytes += 2 * (32 + s.size() * 2);
    }
    return bytes;
}

static QTreeWidgetItem *item(const QString &text, QTreeWidgetItem *parent = nullptr)
{
    auto *i = new QTreeWidgetItem();
    i->setText(0, text);
    if (parent) parent->addChild(i);
    return i;
}

static QString idText(uint32_t id, DBCHandler *dbc)
{
    DBC_MESSAGE *msg = dbc ? dbc->findMessage(id) : nullptr;
    return msg ? Utility::formatHexNum(id) + " (" + msg->name + ")" : Utility::formatHexNum(id);
}

QList<QTreeWidgetItem *> buildCompareTree(const CompareSide &interestedSide, const CompareSide &referenceSide,
                                          const QString &interestedName, bool uniqueInterested, DBCHandler *dbc)
{
    const QString onlyInterested = "only in " + interestedName;
    const QString onlyReference = QStringLiteral("only in Side 2 - Reference frames");
    QTreeWidgetItem *interestedOnlyBase = item("IDs found " + onlyInterested);
    QTreeWidgetItem *referenceOnlyBase = uniqueInterested ? nullptr : item("IDs found " + onlyReference);
    QTreeWidgetItem *sharedBase = item(QStringLiteral("IDs found on both sides"));

    for (auto i = interestedSide.ids.constBegin(); i != interestedSide.ids.constEnd(); ++i) {
        const uint32_t key = i.key();
        auto r = referenceSide.ids.constFind(key);
        if (r == referenceSide.ids.constEnd()) {
            item(idText(key, dbc), interestedOnlyBase);
            continue;
        }
        // the ID was in both files: bits and values seen on one side only
        const FrameData &interested = i.value();
        const FrameData &reference = r.value();
        bool interestedHadUnique = false;
        QTreeWidgetItem *sharedItem = item(idText(key, dbc));
        QTreeWidgetItem *bitsInterested = item("Bits set " + onlyInterested, sharedItem);
        QTreeWidgetItem *bitsReference = uniqueInterested ? nullptr : item("Bits set " + onlyReference, sharedItem);
        const int len = qMax(interested.dataLen, reference.dataLen);
        for (int b = 0; b < 8 * len; b++) {
            const bool inI = interested.bitmap.test(size_t(b)), inR = reference.bitmap.test(size_t(b));
            if (inI == inR) continue;
            const QString text = QString::number(b) + " (" + QString::number(b / 8) + ":" + QString::number(b % 8) + ")";
            if (inI) {
                item(text, bitsInterested);
                interestedHadUnique = true;
            } else if (bitsReference) {
                item(text, bitsReference);
            }
        }
        for (int byte = 0; byte < len; byte++) {
            QTreeWidgetItem *valuesBase = item("Byte " + QString::number(byte), sharedItem);
            QTreeWidgetItem *valuesInterested = item("Values found " + onlyInterested, valuesBase);
            QTreeWidgetItem *valuesReference = uniqueInterested ? nullptr : item("Values found " + onlyReference, valuesBase);
            const std::bitset<256> &vi = interested.values[size_t(byte)], &vr = reference.values[size_t(byte)];
            for (int v = 0; v < 256; v++) {
                if (vi.test(size_t(v)) == vr.test(size_t(v))) continue;
                if (vi.test(size_t(v))) {
                    item(Utility::formatHexNum(static_cast<unsigned int>(v)), valuesInterested);
                    interestedHadUnique = true;
                } else if (valuesReference) {
                    item(Utility::formatHexNum(static_cast<unsigned int>(v)), valuesReference);
                }
            }
        }
        // SavvyCAN: the reference's signals, each with the values found on one side only
        for (auto it = reference.signalInstances.constBegin(); it != reference.signalInstances.constEnd(); ++it) {
            QTreeWidgetItem *valuesBase = item("Signal " + it.key(), sharedItem);
            QTreeWidgetItem *valuesInterested = item("Values found " + onlyInterested, valuesBase);
            QTreeWidgetItem *valuesReference = uniqueInterested ? nullptr : item("Values found " + onlyReference, valuesBase);
            const QSet<QString> refSeen = reference.signalSeen.value(it.key());
            const QSet<QString> intSeen = interested.signalSeen.value(it.key());
            if (valuesReference)
                for (const QString &s : it.value())
                    if (!intSeen.contains(s)) item(s, valuesReference);
            for (const QString &s : interested.signalInstances.value(it.key()))
                if (!refSeen.contains(s)) item(s, valuesInterested);
            if (reference.signalsCapped.contains(it.key()) || interested.signalsCapped.contains(it.key()))
                item(QStringLiteral("(more than %1 values: the rest not compared)").arg(CompareSide::kMaxSignalValues), valuesBase);
        }
        if (interestedHadUnique || !uniqueInterested) sharedBase->addChild(sharedItem);
        else delete sharedItem;
    }
    if (referenceOnlyBase)
        for (auto r = referenceSide.ids.constBegin(); r != referenceSide.ids.constEnd(); ++r)
            if (!interestedSide.ids.contains(r.key())) item(idText(r.key(), dbc), referenceOnlyBase);

    QList<QTreeWidgetItem *> top{interestedOnlyBase};
    if (referenceOnlyBase) top.append(referenceOnlyBase);
    top.append(sharedBase);
    return top;
}

// ---------------------------------------------------------------------------------------------- DBC Comparison
QList<QTreeWidgetItem *> buildDbcDiff(DBCFile *firstDBC, DBCFile *secondDBC)
{
    firstDBC->sort();
    secondDBC->sort();

    // nodes missing in one of the files
    QTreeWidgetItem *nodeDiffRoot = item(QStringLiteral("Node Differences"));
    QTreeWidgetItem *nodesMissingDBCFirst = item(QStringLiteral("Nodes not found in first DBC"), nodeDiffRoot);
    QTreeWidgetItem *nodesMissingDBCSecond = item(QStringLiteral("Nodes not found in second DBC"), nodeDiffRoot);
    for (DBC_NODE *node : firstDBC->dbc_nodes)
        if (!secondDBC->findNodeByName(node->name)) item(node->name, nodesMissingDBCSecond);
    for (DBC_NODE *node : secondDBC->dbc_nodes)
        if (!firstDBC->findNodeByName(node->name)) item(node->name, nodesMissingDBCFirst);

    // messages missing in one of the files; in both: signals missing on one side, or changed
    QTreeWidgetItem *msgDiffRoot = item(QStringLiteral("Message Differences"));
    QTreeWidgetItem *msgMissingDBCFirst = item(QStringLiteral("Messages not found in first DBC"), msgDiffRoot);
    QTreeWidgetItem *msgMissingDBCSecond = item(QStringLiteral("Messages not found in second DBC"), msgDiffRoot);
    QTreeWidgetItem *msgSignalsDiff = item(QStringLiteral("Messages with missing signals"), msgDiffRoot);
    QTreeWidgetItem *sigDiffOne = item(QStringLiteral("Missing from first DBC"), msgSignalsDiff);
    QTreeWidgetItem *sigDiffTwo = item(QStringLiteral("Missing from second DBC"), msgSignalsDiff);
    QTreeWidgetItem *sigModifiedRoot = item(QStringLiteral("Modified Signals"), msgDiffRoot);

    auto msgText = [](DBC_MESSAGE *m) { return m->name + " (" + Utility::formatCANID(m->ID) + ")"; };
    auto change = [](const QString &what, const QString &a, const QString &b) {
        return what + "First DBC: " + a + "      Second: " + b;
    };

    for (DBC_MESSAGE *thisMsg : firstDBC->messageHandler->getMsgsAsList()) {
        DBC_MESSAGE *otherMsg = secondDBC->messageHandler->findMsgByName(thisMsg->name);
        if (!otherMsg) {
            item(msgText(thisMsg), msgMissingDBCSecond);
            continue;
        }
        QTreeWidgetItem *missingItem = nullptr, *modifiedItem = nullptr;
        for (DBC_SIGNAL *thisSig : thisMsg->sigHandler->getSignalsAsList()) {
            if (!thisSig) continue;
            DBC_SIGNAL *otherSig = otherMsg->sigHandler->findSignalByName(thisSig->name);
            if (!otherSig) {
                if (!missingItem) missingItem = item(msgText(thisMsg), sigDiffTwo);
                item(thisSig->name, missingItem);
                continue;
            }
            // on both sides: has it moved, or changed its length or scaling?
            QStringList changes;
            if (thisSig->startBit != otherSig->startBit)
                changes << change("Start Bit       ", QString::number(thisSig->startBit), QString::number(otherSig->startBit));
            if (thisSig->signalSize != otherSig->signalSize)
                changes << change("Size          ", QString::number(thisSig->signalSize), QString::number(otherSig->signalSize));
            if (thisSig->bias != otherSig->bias)
                changes << change("Bias          ", QString::number(thisSig->bias), QString::number(otherSig->bias));
            if (thisSig->factor != otherSig->factor)
                changes << change("Factor        ", QString::number(thisSig->factor), QString::number(otherSig->factor));
            if (changes.isEmpty()) continue;
            if (!modifiedItem) modifiedItem = item(msgText(thisMsg), sigModifiedRoot);
            QTreeWidgetItem *sigItem = item(thisSig->name, modifiedItem);
            for (const QString &c : changes) item(c, sigItem);
        }
        QTreeWidgetItem *missingFirstItem = nullptr;
        for (DBC_SIGNAL *otherSig : otherMsg->sigHandler->getSignalsAsList()) {
            if (!otherSig || thisMsg->sigHandler->findSignalByName(otherSig->name)) continue;
            if (!missingFirstItem) missingFirstItem = item(msgText(thisMsg), sigDiffOne);
            item(otherSig->name, missingFirstItem);
        }
    }
    for (DBC_MESSAGE *msg : secondDBC->messageHandler->getMsgsAsList())
        if (!firstDBC->messageHandler->findMsgByName(msg->name)) item(msgText(msg), msgMissingDBCFirst);

    return {nodeDiffRoot, msgDiffRoot};
}

// ------------------------------------------------------------------------------------------------- Frame Search
bool parseSearchValue(const QString &text, qint64 &integer, double &number, bool &isInteger)
{
    QString t = text.trimmed();
    bool neg = false;
    if (t.startsWith('-')) { neg = true; t = t.mid(1).trimmed(); }
    bool ok = false;
    quint64 u = 0;
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) u = t.mid(2).toULongLong(&ok, 16);
    else if (t.startsWith(QLatin1String("0b"), Qt::CaseInsensitive)) u = t.mid(2).toULongLong(&ok, 2);
    else u = t.toULongLong(&ok, 10);
    if (ok) {
        isInteger = true;
        integer = neg ? -qint64(u) : qint64(u);
        number = double(integer);
        return true;
    }
    isInteger = false;
    number = text.trimmed().toDouble(&ok);
    return ok;
}

int signalBytesNeeded(int startBit, int numBits, bool intel)
{
    if (startBit < 0 || numBits < 1) return 0;
    int highest = 0, bit = startBit;
    for (int i = 0; i < numBits; i++) {
        if (bit < 0 || bit >= kMaxBits) return 0;
        highest = qMax(highest, bit / 8);
        if (intel) bit++;
        else bit = (bit % 8) == 0 ? bit + 15 : bit - 1;
    }
    return highest + 1;
}

FrameSearch::FrameSearch(const SearchCriteria &criteria) : c(criteria)
{
    c.numBits = qBound(1, c.numBits, 64);
    c.startBit = qBound(0, c.startBit, kMaxBits - 1);
    if (qFuzzyIsNull(c.scale)) c.scale = 1.0;
    mBytesNeeded = signalBytesNeeded(c.startBit, c.numBits, c.intel);
    if (c.valueMode == SearchCriteria::Specific) {
        bool isInteger = false;
        mValid = parseSearchValue(c.valueText, mRawTarget, mTarget, isInteger);
        mExact = isInteger && !c.signal && c.scale == 1.0 && c.bias == 0.0;
    }
    if (c.valueMode == SearchCriteria::AnyChange) mExact = !c.signal;
}

bool FrameSearch::matches(const CommFrame &f)
{
    if (c.useID && f.frameId() != c.id) return false;
    const QByteArray payload = f.payload();
    if (c.useLen && payload.size() != c.len) return false;
    if (c.direction == 1 && !f.isReceived()) return false;
    if (c.direction == 2 && f.isReceived()) return false;
    if (c.valueMode == SearchCriteria::NoFilter) return true;

    // the value: the frame must hold the whole signal
    qint64 raw = 0;
    double val = 0;
    if (c.signal) {
        if (!c.signal->processAsDouble(f, val)) return false;
    } else {
        if (mBytesNeeded == 0 || payload.size() < mBytesNeeded) return false;
        // (a 64-bit signed signal: Utility's sign extension would shift by 64; the bits are the value as they are)
        raw = Utility::processIntegerSignal(payload, c.startBit, c.numBits, c.intel, c.isSigned && c.numBits < 64);
        val = double(raw) * c.scale + c.bias;
    }
    if (c.valueMode == SearchCriteria::Specific) {
        if (!mValid) return false;
        if (mExact) {
            if (raw == mRawTarget) return true;
            // a bit pattern typed for a signed signal: 0xFF finds -1 of an 8-bit signal
            const quint64 mask = c.numBits >= 64 ? ~0ULL : ((1ULL << c.numBits) - 1);
            return c.isSigned && mRawTarget >= 0 && (quint64(raw) & mask) == quint64(mRawTarget);
        }
        return val == mTarget || qFuzzyCompare(val, mTarget);
    }
    // any change, per ID
    const uint32_t key = f.frameId();
    if (mExact) {
        auto it = mLastRaw.find(key);
        if (it != mLastRaw.end() && it.value() == raw) return false;
        mLastRaw.insert(key, raw);
    } else {
        auto it = mLast.find(key);
        if (it != mLast.end() && (it.value() == val || qFuzzyCompare(it.value(), val))) return false;
        mLast.insert(key, val);
    }
    return true;
}

} // namespace FileTools
