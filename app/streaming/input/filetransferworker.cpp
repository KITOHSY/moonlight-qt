#include "filetransferworker.h"
#include <QDebug>
#include <QThread>

FileTransferWorker::FileTransferWorker(const QString& filePath, QObject* parent)
    : QObject(parent), m_FilePath(filePath) {}

void FileTransferWorker::process()
{
    qDebug() << "[스레드]" << QThread::currentThread() << "에서 처리 중:" << m_FilePath;


    // TODO: 여기서 파일 전송, 서버 업로드 등 무거운 로직 처리
    QThread::sleep(1);  // 예제용 지연

    emit finished();
}
