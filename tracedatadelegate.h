#ifndef TRACEDATADELEGATE_H
#define TRACEDATADELEGATE_H

#include <QStyledItemDelegate>

// web-savvycan: the main trace's Data column for CAN FD. A frame of up to 64 bytes is wrapped at
// "Maximum data bytes per line" (Preferences); a row at the normal height shows the first line only,
// so this delegate draws a small "+N B" badge at the right of a cell that has bytes out of sight.
// Clicking the row (or Expand All Rows) shows them all, as in SavvyCAN; the cell's tooltip has the
// whole payload with byte offsets.
class TraceDataDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit TraceDataDelegate(QObject *parent = nullptr);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

#endif // TRACEDATADELEGATE_H
