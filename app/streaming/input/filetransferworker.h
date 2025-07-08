#pragma once
#include <QObject>
#include <QString>

class FileTransferWorker : public QObject
{
    Q_OBJECT
public:
    explicit FileTransferWorker(const QString& filePath, QObject* parent = nullptr);

public slots:
    void process();  // 이 안에서 파일 전송 처리

signals:
    void finished();

private:
    QString m_FilePath;
};
