#ifndef TRACEROWHEADER_H
#define TRACEROWHEADER_H

/*
 * web-savvycan: the trace's vertical header (row numbers).
 *
 * When rows are removed from the top of the trace (the frame ring wrapping) while the view is
 * scrolled to the bottom, the header's scroll offset is briefly past its new length. Qt's
 * QHeaderView::paintEvent() then gets -1 from visualIndexAt() for the top of the update rect and
 * falls back to painting from section 0, i.e. every one of the (up to millions of) rows: 360 ms per
 * paint with 40 000 rows in tst_ingest_load. Nothing of that is visible, so this header skips such
 * a paint and asks for another one once the view has caught up (the next event-loop turn; the
 * scroll and geometry updates that follow repaint it anyway).
 */

#include <QHeaderView>
#include <QPaintEvent>
#include <QTimer>

class TraceRowHeader : public QHeaderView
{
public:
    explicit TraceRowHeader(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QHeaderView(orientation, parent) {}

protected:
    void paintEvent(QPaintEvent *e) override
    {
        const int top = orientation() == Qt::Vertical ? e->rect().top() : e->rect().left();
        if (count() > 0 && visualIndexAt(top) < 0)
        {
            //nothing of the header is in view; look again on the next turns (a few times, never a loop)
            if (!mRepaintQueued && mSkipped++ < 3)
            {
                mRepaintQueued = true;
                QTimer::singleShot(0, this, [this] { mRepaintQueued = false; viewport()->update(); });
            }
            return;
        }
        mSkipped = 0;
        QHeaderView::paintEvent(e);
    }

private:
    bool mRepaintQueued = false;
    int mSkipped = 0;
};

#endif // TRACEROWHEADER_H
