#ifndef WEBDISKLOG_H
#define WEBDISKLOG_H

// web-savvycan: continuous logging to a file on disk in the browser (WebAssembly build only).
// A thin Qt front for the JavaScript module wasm/disklog/disklog.js (globalThis.SavvyDiskLog),
// which writes through the File System Access API (Chrome, Edge) or a streaming download
// (Firefox). Everything there is asynchronous: start() returns at once and started() or
// startFailed() follows.

#include <QObject>
#include <QByteArray>
#include <QString>

class WebDiskLog : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        PromptForName,   // the browser's save dialog names the file
        StartTimeName,   // created in the remembered log folder, named by the start time
        Download,        // streaming download (browsers without the File System Access API)
        BlobDownload     // collected in the page, one normal download on stop (the MVP, every browser)
    };
    Q_ENUM(Mode)

    static WebDiskLog *instance();

    // What this browser offers: "fsa" (File System Access), "download" (streaming download), "none"
    QString backend() const;

    // splitBytes (StartTimeName only): continue in <name>_part002.csv, ... once a file has this size
    // suggestedName: the file's name (BlobDownload, Download, PromptForName); empty: by the start time
    void start(Mode mode, const QByteArray &header, qint64 splitBytes = 0, const QString &suggestedName = QString());
    bool write(const QByteArray &rows, int frames = 0);   // false when no log is open
    // downloadName (BlobDownload): the name the download gets; empty: the one it started with
    void stop(const QString &downloadName = QString());

    bool isActive() const { return m_active; }
    bool isStarting() const { return m_starting; }
    QString fileName() const { return m_fileName; }
    QString activeBackend() const { return m_backend; }
    bool crashSafe() const { return m_crashSafe; }
    qint64 committedBytes() const { return m_committedBytes; }
    qint64 lastCommitMSecsSinceEpoch() const { return m_lastCommit; }
    int part() const { return m_part; }
    qint64 bytesWritten() const { return m_bytesWritten; }   // header + rows handed over so far
    qint64 framesWritten() const { return m_framesWritten; }

    // The remembered log folder: answered by directoryInfo()
    void queryDirectory();
    void chooseDirectory();   // needs a user gesture (call it from a click)

    // called from JavaScript (embind), not for the app
    void handleEvent(const QString &type, const QString &name, const QString &code,
                     const QString &message, double bytes, bool crashSafe, const QString &backend, int part);

signals:
    void started(const QString &fileName, const QString &backend, bool crashSafe);
    void startFailed(const QString &code, const QString &message); // code "cancelled": the user said no
    void committed(qint64 bytes);
    void failed(const QString &code, const QString &message, qint64 committedBytes);
    void stopped(const QString &fileName, qint64 bytes);
    void fileChanged(const QString &fileName, int part); // the log went on in a new part
    void directoryInfo(const QString &name, const QString &permission); // name empty: none chosen yet

private:
    explicit WebDiskLog(QObject *parent = nullptr);

    bool m_active = false;
    bool m_starting = false;
    bool m_crashSafe = true;
    QString m_fileName;
    QString m_backend;
    qint64 m_committedBytes = 0;
    qint64 m_lastCommit = 0;
    int m_part = 1;
    qint64 m_bytesWritten = 0;
    qint64 m_framesWritten = 0;
};

#endif // WEBDISKLOG_H
