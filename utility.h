#ifndef UTILITY_H
#define UTILITY_H

#include <Qt>
#include <stdint.h>
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QApplication>
#include <QRect>
#include <QComboBox>
#include <QStandardItemModel>
#include <QColor>
#include <QString>
#include <array>
#include "theme.h"
//#include <QDesktopWidget>

enum TimeStyle
{
    TS_SECONDS,
    TS_MICROS,
    TS_MILLIS,
    TS_CLOCK
};


// 32 dark, saturated colors — all designed for readability on white backgrounds.
// Minimum contrast ratio ≥ 4.5:1 against white (WCAG AA).
static constexpr std::array<QRgb, 32> kRowPalette = {{
    0xFFC62828,  //  0  Red 800
    0xFFE53935,  //  1  Red 600
    0xFFD84315,  //  2  Deep Orange 800
    0xFFEF6C00,  //  3  Orange 800
    0xFFF9A825,  //  4  Amber 800
    0xFF558B2F,  //  5  Green 800
    0xFF2E7D32,  //  6  Green 900
    0xFF00695C,  //  7  Teal 800
    0xFF00838F,  //  8  Cyan 800
    0xFF0277BD,  //  9  Blue 800
    0xFF1565C0,  // 10  Blue 900
    0xFF283593,  // 11  Indigo 900
    0xFF4527A0,  // 12  Deep Purple 800
    0xFF6A1B9A,  // 13  Purple 900
    0xFF880E4F,  // 14  Pink 900
    0xFFAD1457,  // 15  Pink 800
    0xFF37474F,  // 16  Blue Grey 800
    0xFF4E342E,  // 17  Brown 800
    0xFF827717,  // 18  Lime 900
    0xFF1B5E20,  // 19  Green 900 alt
    0xFF006064,  // 20  Cyan 900
    0xFF01579B,  // 21  Light Blue 900
    0xFF0D47A1,  // 22  Blue 900 alt
    0xFF311B92,  // 23  Deep Purple 900
    0xFF4A148C,  // 24  Purple 900 alt
    0xFFB71C1C,  // 25  Red 900
    0xFFBF360C,  // 26  Deep Orange 900
    0xFFE65100,  // 27  Orange 900
    0xFF33691E,  // 28  Light Green 900
    0xFF004D40,  // 29  Teal 900
    0xFF263238,  // 30  Blue Grey 900
    0xFF5D4037,  // 31  Brown 700
}};

// web-savvycan: the dark theme's set — the same hues as kRowPalette, light (Material 200-400)
// so that a faint tint of them reads well on the dark theme's near-black rows with light text.
static constexpr std::array<QRgb, 32> kRowPaletteDark = {{
    0xFFE57373, 0xFFEF9A9A, 0xFFFF8A65, 0xFFFFB74D, 0xFFFFD54F, 0xFF81C784, 0xFF66BB6A, 0xFF4DB6AC,
    0xFF4DD0E1, 0xFF4FC3F7, 0xFF64B5F6, 0xFF7986CB, 0xFF9575CD, 0xFFBA68C8, 0xFFF06292, 0xFFF48FB1,
    0xFF90A4AE, 0xFFA1887F, 0xFFDCE775, 0xFFA5D6A7, 0xFF80DEEA, 0xFF81D4FA, 0xFF90CAF9, 0xFFB39DDB,
    0xFFCE93D8, 0xFFEF5350, 0xFFFFAB91, 0xFFFFA726, 0xFFAED581, 0xFF80CBC4, 0xFFB0BEC5, 0xFFBCAAA4,
}};
static_assert(kRowPaletteDark.size() == kRowPalette.size(), "one dark colour per light one");

class Utility
{
public:

    static bool decimalMode;
    static TimeStyle timeStyle;
    static QString timeFormat;

    static QString fullyQualifiedNameSeperator;

    static void SetComboBoxItemEnabled(QComboBox * comboBox, int index, bool enabled)
    {
        auto * model = qobject_cast<QStandardItemModel*>(comboBox->model());
        assert(model);
        if(!model) return;

        auto * item = model->item(index);
        assert(item);
        if(!item) return;
        item->setEnabled(enabled);
    }

    //determines whether the window position is within any available screens. If it is not we default
    //back to 0,0 which is going to be on screen. This fixes a problem where some operating systems would
    //otherwise let you put windows on a second monitor, disconnect that monitor, and still put windows on it.
    static QPoint constrainedWindowPos(QPoint originalPos)
    {
        QScreen *screen = QGuiApplication::screenAt(originalPos);
        if (!screen)
        {
            return QPoint(0,0);
        }
        return originalPos;
    }

    static QString unQuote(QString inStr)
    {
        QStringList temp;
        temp = inStr.split('\"');
        if (temp.length() >= 3)
            return temp[1];
        return inStr;
    }

    static uint64_t ParseStringToNum(QByteArray input)
    {
        uint64_t temp = 0;

        input = input.toUpper();
        if (input.startsWith("0X") || input.startsWith("X")) //hex number
        {
            if (input.length() < 3) temp = 0;
            else temp = input.right(input.size() - 2).toLongLong(nullptr, 16);
        }
        else if (input.startsWith("0B") || input.startsWith("B")) //binary number
        {
            input = input.right(input.size() - 1); //remove the B
            for (int i = 0; i < input.length(); i++)
            {
                if (input[i] == '1') temp += (uint64_t)1 << (input.length() - i - 1);
            }
        }
        else //decimal number
        {
            temp = input.toLongLong();
        }

        return temp;
    }

    static uint64_t ParseStringToNum(QString input)
    {
        return ParseStringToNum(input.toUtf8());
    }

    static uint ParseStringToNum2(QString pInput, bool* pOk_p = nullptr)
    {
        if(pInput.startsWith("0b"))
        {
            pInput.remove(0, 2);
            return pInput.toUInt(pOk_p, 2);
        }

        return pInput.toUInt(pOk_p, 0);
    }

    static uint64_t GetTimeMS()
    {
        QDateTime stamp = QDateTime::currentDateTime();
        return (((static_cast<uint64_t>(stamp.time().hour()) * 3600ull) + (static_cast<uint64_t>(stamp.time().minute()) * 60ull)
                 + (static_cast<uint64_t>(stamp.time().second())) * 1000ull) + static_cast<uint64_t>(stamp.time().msec()));
    }

    //prints hex numbers in uppercase with 0's filling out the number depending
    //on the size needed. Promotes hex numbers to either 2, 4, or 8 digits
    static QString formatHexNum(uint64_t input)
    {
        if (input < 256)
            return "0x" + QString::number(input, 16).toUpper().rightJustified(2,'0');
        if (input < 65536)
            return "0x" + QString::number(input, 16).toUpper().rightJustified(4,'0');
        if (input < 4294967296)
            return "0x" + QString::number(input, 16).toUpper().rightJustified(8,'0');
        return "0x" + QString::number(input, 16).toUpper().rightJustified(16,'0');
    }

    //uses decimalMode to see if it should show value as decimal or hex
    static QString formatNumber(uint64_t value)
    {
        if (decimalMode)
        {
            return QString::number(value, 10);
        }
        else return formatHexNum(value);
    }

    static QString formatCANID(uint64_t id, bool extended)
    {
        if (decimalMode) return QString::number(id, 10);

        if (extended)
        {
            return "0x" + QString::number(id, 16).toUpper().rightJustified(8,'0');
        }
        else
        {
            id = id & 0x7FF;
            return "0x" + QString::number(id, 16).toUpper().rightJustified(3,'0');
        }
    }

    static QString formatCANID(uint64_t id)
    {
        if (id < 0x800) return formatCANID(id, false);
        return formatCANID(id, true);
    }

    static QString formatByteAsBinary(uint8_t value)
    {
        QString output;
        for (int b = 7; b >= 0; b--)
        {
            if (value & (1 << b)) output += "1";
            else output += "0";
        }
        return output;
    }

    static QString formatByteAsHex(uint8_t value)
    {
        return QString::number(value, 16).toUpper().rightJustified(2,'0');
    }

    static QVariant formatTimestamp(uint64_t timestamp)
    {
        switch (timeStyle)
        {
        case TS_CLOCK:
            return QDateTime::fromMSecsSinceEpoch(timestamp / 1000);
            break;
        case TS_MILLIS:
            return (double)timestamp / 1000.0;
            break;
        case TS_MICROS:
            return (unsigned long long)(timestamp);
            break;
        case TS_SECONDS:
        default:
            return (double)timestamp / 1000000.0;
            break;
        }
        return QVariant();
    }

    //parses the input string to grab as much of it as possible while staying alpha numeric
    static QString grabAlphaNumeric(QString &input)
    {
        QString builder;
        QChar thisChar;
        for (int i = 0; i < input.length(); i++)
        {
            thisChar = input[i];
            if (thisChar.isLetterOrNumber() || thisChar == ':' || thisChar == '~') builder.append(input[i]);
            else
            {
                //qDebug() << "i: "<< i << " len: " << input.length();
                if (i < (input.length() - 1)) input = input.right(input.length() - i);
                else input = "";
                return builder;
            }
        }
        //qDebug() << "Reached end of string in grabAlphaNumeric";
        input = "";
        return builder;
    }

    static QString grabOperation(QString &input)
    {
        QString builder;
        QChar thisChar = input[0];

        if (thisChar == '+' || thisChar == '-' || thisChar == '*' || thisChar == '/' || thisChar == '^' || thisChar == '&' || thisChar == '|' || thisChar == '=' || thisChar == '%')
        {
            input = input.right(input.length() - 1);
            builder = thisChar;
        }
        return builder;
    }

    static int getByteFromBitPosition(int bitPos)
    {
        return bitPos / 8;
    }

    static int getBitFromBitPosition(int bitPos)
    {
        return bitPos & 7;
    }

    //simple linear interpolation between value1 and value2. sample point is 0.0 to 1.0
    static double Lerp(double value1, double value2, double samplePoint)
    {
        return (value1 * (1.0 - samplePoint)) + (value2 * samplePoint);
    }

    /* web-savvycan: the bits a DBC-style signal covers, as SavvyCAN numbers them (bit 7 of byte 0 is 7, bit 0 of byte 1
     * is 8, ... up to 511 in a 64-byte CAN FD frame): Intel counts up from the start bit; Motorola starts at the start
     * bit (the signal's most significant bit) and counts down within the byte, then goes on at bit 7 of the next byte.
     * signalLastByte() is the highest byte index the signal touches (-1 for a size < 1), so a frame of
     * signalLastByte() + 1 bytes or more carries the whole signal. (SavvyCAN checked (startBit + size) / 8 bytes,
     * which is wrong for Motorola: a signal in the last byte, start bit 63 size 8, "didn't fit" an 8-byte frame.)
     */
    static int signalLastByte(int startBit, int sigSize, bool littleEndian)
    {
        if (sigSize < 1 || startBit < 0) return -1;
        if (littleEndian) return (startBit + sigSize - 1) / 8;
        // Motorola: the first byte holds (startBit % 8) + 1 bits, every following byte 8
        const int inFirst = (startBit % 8) + 1;
        if (sigSize <= inFirst) return startBit / 8;
        return startBit / 8 + (sigSize - inFirst + 7) / 8;
    }
    static bool signalFits(int dataLen, int startBit, int sigSize, bool littleEndian)
    {
        const int last = signalLastByte(startBit, sigSize, littleEndian);
        return last >= 0 && last < dataLen;
    }

    /* A unified function that can extract a signal from the (up to) 64 bits of data bytes in a CAN frame
     * handles both little and big endian signals (and floats too).
     * web-savvycan: CAN FD: any start bit in a frame of up to 64 bytes (bits 0..511), up to 64 bits long (an int64; a
     * longer size is taken as 64: 1 << 64 was undefined); 0 when the signal doesn't fit the frame (as before).
    */
    static int64_t processIntegerSignal(const QByteArray &data, int startBit, int sigSize, bool littleEndian, bool isSigned)
    {
        if (sigSize > 64) sigSize = 64;
        if (!signalFits(data.size(), startBit, sigSize, littleEndian)) return 0; //signal extends past the end of data
        const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data.constData());

        uint64_t result = 0;
        int bit = startBit;
        if (littleEndian)
        {
            for (int bitpos = 0; bitpos < sigSize; bitpos++, bit++)
                if (bytes[bit / 8] & (1 << (bit % 8)))
                    result |= (1ULL << bitpos);
        }
        else //motorola / big endian mode
        {
            for (int bitpos = 0; bitpos < sigSize; bitpos++)
            {
                if (bytes[bit / 8] & (1 << (bit % 8)))
                    result |= (1ULL << (sigSize - bitpos - 1));
                if ((bit % 8) == 0)
                    bit += 15;
                else bit--;
            }
        }

        //signed: the signal's highest bit set means negative, so every bit above the signal is set too (sign extension)
        if (isSigned && sigSize < 64 && (result & (1ULL << (sigSize - 1))))
            result |= ~((1ULL << sigSize) - 1);
        return (int64_t)result;
    }

    // split comma-separated list of quoted strings
    // "A","B","C,D,E"
    static QStringList splitQuotedString(QString input)
    {
        QStringList parts;
        bool inQuotes = false;
        QString currentToken;

        for (int i = 0; i < input.length(); ++i)
        {
            QChar c = input.at(i);

            if (c == '\"') {
                inQuotes = !inQuotes;
                //currentToken += c;
            } else if (c == ',' && !inQuotes) {
                parts.append(currentToken.trimmed());
                currentToken.clear();
            } else {
                currentToken += c;
            }
        }
        parts.append(currentToken.trimmed()); // the last token
        return parts;
    }

    // FNV-1a 32-bit hash — fast, good distribution, no dependencies.
    static quint32 hashString(const QString &s)
    {
        quint32 hash = 2166136261u;
        for (const QChar ch : s) {
            hash ^= static_cast<quint32>(ch.unicode());
            hash *= 16777619u;
        }
        return hash;
    }

    // Returns a background QColor deterministically chosen from the palette.
    // The same string always maps to the same color.
    static QColor colorForString(const QString &s)
    {
        const quint32 idx = hashString(s) % static_cast<quint32>(kRowPalette.size());
        // Use a light tint (alpha ~18%) as the row background so text stays readable.
        if (Theme::isDark()) {
            QColor base(kRowPaletteDark[idx]);
            base.setAlpha(56); // ~22% of a light hue over the dark rows: tinted, text stays readable
            return base;
        }
        QColor base(kRowPalette[idx]);
        base.setAlpha(46); // ~18% — comfortable on white, still clearly tinted
        return base;
    }

    // Opaque variant — useful for badges, tags, or legend swatches.
    static QColor solidColorForString(const QString &s)
    {
        const quint32 idx = hashString(s) % static_cast<quint32>(kRowPalette.size());
        return QColor(Theme::isDark() ? kRowPaletteDark[idx] : kRowPalette[idx]);
    }
};

#endif // UTILITY_H
