#include "cli/StdinReader.h"

#include <QByteArray>

#include <cstdio>

#ifdef Q_OS_WIN
#  include <fcntl.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace swt::edl {

namespace {
constexpr int kChunk = 64 * 1024;
}

StdinReader::StdinReader(QObject* parent)
    : QObject(parent)
{
}

void StdinReader::run()
{
#ifdef Q_OS_WIN
    // Windows 上必须切成二进制：文本模式下 CRLF 会被改写、0x1A 还会被当成 EOF，
    // 于是「最后一条指令偶尔丢」这种间歇故障就来了。
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    QByteArray pending;
    char buffer[kChunk];
    while (true) {
        // 🔴 这里**不能**用 `std::fread(buffer, 1, kChunk, stdin)`。
        //    fread 的契约是「读满 nmemb 个 size 字节的项」：size=1 / nmemb=65536 时，
        //    实现会一直循环到**读满 65536 字节或遇到 EOF**才返回。
        //    后果：父进程保持管道打开、只写一行指令时，fread 卡住不返回，
        //    一条指令都处理不了 —— 直到 stdin 被关闭(EOF)才把攒下的行一次吐出。
        //    实测：`printf '…' | SWT-EDL.exe --cli`（printf 立刻退出=EOF）看起来「正常」，
        //    但 Python/子进程常驻驱动时 ping 永远等不到 pong（probe 7 条 FAIL 全是这个）。
        //    原始 fd 读是「有多少读多少」，有数据立即返回，才是行协议的语义。
#ifdef Q_OS_WIN
        const int got = _read(_fileno(stdin), buffer, static_cast<unsigned>(sizeof(buffer)));
#else
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
#endif
        if (got <= 0) break;
        pending.append(buffer, static_cast<int>(got));

        // 逐行切；**只按 \n 切**，并把行尾的 \r 去掉（Windows 写过来的行就是 \r\n）
        int start = 0;
        while (true) {
            const int nl = pending.indexOf('\n', start);
            if (nl < 0) break;
            QByteArray raw = pending.mid(start, nl - start);
            if (raw.endsWith('\r')) raw.chop(1);
            emit lineReceived(QString::fromUtf8(raw));
            start = nl + 1;
        }
        if (start > 0) pending.remove(0, start);
    }

    // 文件尾可能还有一行没有换行符结尾
    if (!pending.isEmpty()) {
        if (pending.endsWith('\r')) pending.chop(1);
        emit lineReceived(QString::fromUtf8(pending));
    }
    emit eof();
}

} // namespace swt::edl
