#include "tracedatadelegate.h"
#include "canframemodel.h"

#include <QPainter>
#include <QApplication>

TraceDataDelegate::TraceDataDelegate(QObject *parent) : QStyledItemDelegate(parent)
{
}

void TraceDataDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyledItemDelegate::paint(painter, option, index);

    const QString text = index.data(Qt::DisplayRole).toString();
    if (text.isEmpty()) return;

    // Only the byte lines count: anything after a DBC interpretation ("   <name>") is not data.
    const QStringList lines = text.split(QLatin1Char('\n'));
    const QFontMetrics fm(option.font);
    const int visibleLines = qMax(1, option.rect.height() / qMax(1, fm.lineSpacing()));
    if (lines.size() <= visibleLines) return;

    const int totalBytes = index.siblingAtColumn((int)Column::Length).data(Qt::DisplayRole).toInt();
    int shownBytes = 0;
    for (int i = 0; i < visibleLines && i < lines.size(); ++i)
        shownBytes += lines[i].split(QLatin1Char(' '), Qt::SkipEmptyParts).size();
    const int hiddenBytes = totalBytes - shownBytes;
    if (hiddenBytes <= 0) return;

    const QString badge = QStringLiteral("+%1 B").arg(hiddenBytes);
    QFont badgeFont = option.font;
    badgeFont.setPointSizeF(qMax(6.0, badgeFont.pointSizeF() * 0.85));
    const QFontMetrics bfm(badgeFont);
    const int w = bfm.horizontalAdvance(badge) + 8;
    const int h = qMin(option.rect.height() - 2, bfm.height() + 2);
    QRect r(option.rect.right() - w - 3, option.rect.top() + (option.rect.height() - h) / 2, w, h);

    const QPalette &pal = option.palette;
    const bool selected = option.state & QStyle::State_Selected;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QColor fill = selected ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Highlight);
    fill.setAlpha(selected ? 60 : 45);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(r, 4, 4);
    painter->setFont(badgeFont);
    painter->setPen(selected ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Text));
    painter->drawText(r, Qt::AlignCenter, badge);
    painter->restore();
}
