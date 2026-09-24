// fcitx5 微信输入法 addon — 异步事件驱动版 (2026-09-10)
//
// 架构: fcitx5 主线程 IO 事件驱动, 全程零阻塞:
//   keyEvent → 写 "L c" 到引擎 stdin(内核缓冲, 不等) + 本地即时回显 buf_
//   引擎响应(CAND)通过 EventLoop IO 事件到达 → parseCand → 更新候选面板
//   响应与命令按 FIFO handler 队列配对, 迟到/乱序不可能发生
//
// 引擎目录解析(按序): $WETYPE_ENGINE_DIR > ~/.local/lib/wetype-ime/arm64 >
//   /usr/lib/wetype-ime/arm64 >
//   ~/wetype-ime/squashfs-root/usr/lib/wetype-ime/arm64(开发回退)
#include <fcitx/instance.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/candidatelist.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/trackableobject.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_set>

namespace fcitx {

static constexpr int GRID_ROWS = 4;
static constexpr int GRID_COLUMNS = 5;
// Compact bar is one horizontal row. It is not tied to the expanded grid.
static constexpr int COMPACT_PAGE_SIZE = 7;
static constexpr int PAGE_SIZE = GRID_ROWS * GRID_COLUMNS;
// Same line box as a CJK candidate, so the panel does not grow when words arrive.
static const char *const CANDIDATE_ROW_PAD = "\u3000";
static constexpr uint64_t CANDIDATE_DEBOUNCE_USEC = 40000;
static constexpr size_t CANDIDATE_DEBOUNCE_MAX_CHARS = 4;

static uint64_t monotonicUsec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + ts.tv_nsec / 1000;
}

// Display-only segmentation. The original unsegmented buffer is still sent to
// the WeType engine, so this never changes composition or candidate matching.
static std::string segmentPinyin(const std::string &raw) {
    static const std::unordered_set<std::string> syllables = [] {
        static const char *data =
            "a ai an ang ao e ei en eng er o ou "
            "ba bai ban bang bao bei ben beng bi bian biao bie bin bing bo bu "
            "pa pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu "
            "ma mai man mang mao me mei men meng mi mian miao mie min ming miu mo mou mu "
            "fa fan fang fei fen feng fo fou fu "
            "da dai dan dang dao de dei den deng di dia dian diao die ding diu dong dou du duan dui dun duo "
            "ta tai tan tang tao te teng ti tian tiao tie ting tong tou tu tuan tui tun tuo "
            "na nai nan nang nao ne nei nen neng ng ni nian niang niao nie nin ning niu nong nou nu nuan nue nuo nv nve "
            "la lai lan lang lao le lei leng li lia lian liang liao lie lin ling liu long lou lu luan lue lun luo lv "
            "ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan guang gui gun guo "
            "ka kai kan kang kao ke kei ken keng kong kou ku kua kuai kuan kuang kui kun kuo "
            "ha hai han hang hao he hei hen heng hong hou hu hua huai huan huang hui hun huo "
            "za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe zhei zhen zheng zhi zhong zhou zhu zhua zhuai zhuan zhuang zhui zhun zhuo zi zong zou zu zuan zui zun zuo "
            "ca cai can cang cao ce cen ceng cha chai chan chang chao che chen cheng chi chong chou chu chua chuai chuan chuang chui chun chuo ci cong cou cu cuan cui cun cuo "
            "sa sai san sang sao se sen seng sha shai shan shang shao she shei shen sheng shi shou shu shua shuai shuan shuang shui shun shuo si song sou su suan sui sun suo "
            "ra ran rang rao re ren reng ri rong rou ru ruan rui run ruo "
            "ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun "
            "qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun "
            "xi xia xian xiang xiao xie xin xing xiong xiu xu xuan xue xun "
            "ya yan yang yao ye yi yin ying yo yong you yu yuan yue yun "
            "wa wai wan wang wei wen weng wo wu "
            "bo bei ben beng bian bie bin bing "
            "ge gei gen geng gong gou gua guai guan guang gui gun guo "
            "jiong juan jue jun";
        std::unordered_set<std::string> result;
        std::istringstream input(data);
        std::string item;
        while (input >> item) result.insert(item);
        return result;
    }();

    std::string out;
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '\'') {
            out += raw[i++];
            continue;
        }
        size_t match = 0;
        const size_t limit = std::min(raw.size(), i + 6);
        for (size_t end = limit; end > i; --end) {
            if (raw.find('\'', i) < end) continue;
            if (syllables.count(raw.substr(i, end - i))) {
                match = end - i;
                break;
            }
        }
        if (match) {
            if (!out.empty() && out.back() != '\'' && out.back() != ' ') out += ' ';
            out.append(raw, i, match);
            i += match;
        } else {
            if (!out.empty() && out.back() != '\'' && out.back() != ' ') out += ' ';
            out.append(raw, i, std::string::npos);
            break;
        }
    }
    return out;
}

static Text pinyinPreedit(const std::string &buffer) {
    Text text;
    if (!buffer.empty()) text.append(segmentPinyin(buffer), TextFormatFlag::NoFlag);
    return text;
}

struct GridColumnCandidate : public CandidateWord {
    GridColumnCandidate(Text text, std::function<void(InputContext *)> select)
        : CandidateWord(std::move(text)), select_(std::move(select)) {
        setCustomLabel(Text(""));
    }
    void select(InputContext *ic) const override {
        if (select_) select_(ic);
    }
private:
    std::function<void(InputContext *)> select_;
};
// 调试日志: 进 fcitx5 的 stderr, 不影响功能
#define WLOG(...) do { fprintf(stderr, "[wetype-addon] " __VA_ARGS__); fflush(stderr); } while (0)

// ---------------------------------------------------------------- 引擎目录解析
static void resolveDirs(std::string &eng, std::string &dicts, std::string &work,
                        std::string &qemu, std::string &sysroot) {
    auto existsEngine = [](const std::string &p) {
        return ::access((p + "/wetype-harness").c_str(), X_OK) == 0 &&
               ::access((p + "/lib/libwxhld_jni.so").c_str(), R_OK) == 0;
    };
    const char *home = getenv("HOME") ? getenv("HOME") : "/root";
    eng = getenv("WETYPE_ENGINE_DIR") ? getenv("WETYPE_ENGINE_DIR") : "";
    if (eng.empty() || !existsEngine(eng)) {
        eng = std::string(home) + "/.local/lib/wetype-ime/arm64";
        if (!existsEngine(eng)) {
            eng = "/usr/lib/wetype-ime/arm64";
            if (!existsEngine(eng)) {
                eng = std::string(home) + "/wetype-ime/squashfs-root/usr/lib/wetype-ime/arm64";  // 开发回退
            }
        }
    }
    dicts = getenv("WETYPE_DICT_DIR") ? getenv("WETYPE_DICT_DIR") : eng + "/dicts";
    // QEMU 与 ARM64 glibc 随安装一起提供；缺失时回落到系统包
    qemu = getenv("QEMU_AARCH64") ? getenv("QEMU_AARCH64") : "";
    if (qemu.empty())
        qemu = ::access((eng + "/qemu-aarch64-static").c_str(), X_OK) == 0
                   ? eng + "/qemu-aarch64-static" : "qemu-aarch64-static";
    sysroot = getenv("WETYPE_SYSROOT") ? getenv("WETYPE_SYSROOT") : "";
    if (sysroot.empty())
        sysroot = ::access((eng + "/sysroot/lib/ld-linux-aarch64.so.1").c_str(), R_OK) == 0
                      ? eng + "/sysroot" : "/usr/aarch64-linux-gnu";
    const char *xdg = getenv("XDG_DATA_HOME");
    std::string base = xdg && *xdg ? xdg : std::string(home) + "/.local/share";
    work = getenv("WETYPE_WORK_DIR") ? getenv("WETYPE_WORK_DIR") : base + "/wetype-ime/dict";
}

// ---------------------------------------------------------------- 异步引擎进程
class EngineProc {
public:
    using Handler = std::function<void(const std::string &)>;   // "" = 失败/死亡

    explicit EngineProc(EventLoop &loop) : loop_(loop) {}
    ~EngineProc() { stop(); }

    bool alive() const { return pid_ > 0; }
    bool ready() const { return state_ == State::Ready; }
    bool gaveUp() const { return gaveUp_; }
    void setOnReady(std::function<void()> cb) { onReady_ = std::move(cb); }

    // 非阻塞启动; 就绪走 IO 事件。指数退避防 fork 风暴(连续 5 次失败放弃)。
    bool start() {
        if (pid_ > 0) return true;
        if (failCount_ >= 5) {
            if (!gaveUp_) { WLOG("engine gave up after %d failures\n", failCount_); gaveUp_ = true; }
            return false;
        }
        std::string eng, dicts, work, qemu, sysroot;
        resolveDirs(eng, dicts, work, qemu, sysroot);

        int inP[2], outP[2];
        if (pipe2(inP, O_CLOEXEC) < 0) return false;
        if (pipe2(outP, O_CLOEXEC) < 0) {
            close(inP[0]);
            close(inP[1]);
            return false;
        }
        pid_t p = fork();
        if (p < 0) {
            close(inP[0]); close(inP[1]); close(outP[0]); close(outP[1]);
            return false;
        }
        if (p == 0) {
            // QEMU user mode writes guest core files in its current directory.
            // Keep an engine crash from leaving qemu_wetype-harness_*.core in HOME.
            const struct rlimit noCore = {0, 0};
            if (setrlimit(RLIMIT_CORE, &noCore) < 0) _exit(127);
            dup2(inP[0], 0);
            dup2(outP[1], 1);
            close(inP[0]);
            close(inP[1]);
            close(outP[0]);
            close(outP[1]);
            const char *logPath = getenv("WETYPE_HARNESS_LOG");
            if (!logPath || !*logPath) logPath = "/tmp/wetype-harness.log";
            int logFd = open(logPath, O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (logFd >= 0) { dup2(logFd, 2); close(logFd); }
            setenv("LD_LIBRARY_PATH", (eng + "/lib").c_str(), 1);
            setenv("WETYPE_LIB_DIR",  (eng + "/lib").c_str(), 1);
            setenv("WETYPE_DICT_DIR", dicts.c_str(), 1);
            setenv("WETYPE_ASSET_DIR", dicts.c_str(), 1);
            setenv("WETYPE_WORK_DIR", work.c_str(), 1);
            execlp(qemu.c_str(), qemu.c_str(), "-L", sysroot.c_str(),
                   (eng + "/wetype-harness").c_str(),
                   (eng + "/lib/libwxhld_jni.so").c_str(), "--daemon", (char *)nullptr);
            _exit(127);
        }
        close(inP[0]);
        close(outP[1]);
        pid_ = p;  in_ = inP[1];  out_ = outP[0];
        rbuf_.clear();
        state_ = State::Starting;
        ++failCount_;
        ioSource_ = loop_.addIOEvent(
            out_, IOEventFlags(IOEventFlag::In),
            [this](EventSourceIO *, int fd, IOEventFlags) { onReadable(fd); return true; });
        // 启动看门狗: 90s 未 READY 判失败
        timeSource_ = loop_.addTimeEvent(
            CLOCK_MONOTONIC, nowUsec() + 90ull * 1000000ull, 0,
            [this](EventSourceTime *, uint64_t) { onStartTimeout(); return true; });
        WLOG("engine starting (attempt %d) eng=%s\n", failCount_, eng.c_str());
        return true;
    }

    void stop() {
        WLOG("stop() called (pid=%d)\n", pid_);
        if (pid_ > 0) {
            if (in_ >= 0) {
                ssize_t w = write(in_, "Q\n", 2);
                (void)w;
                close(in_);
                in_ = -1;
            }
            int st = 0;
            bool exited = false;
            // Let the harness process Q, destroy its engine session and flush
            // learned words. Bound shutdown so a wedged QEMU cannot stall Fcitx.
            for (int i = 0; i < 100; ++i) {
                pid_t result = waitpid(pid_, &st, WNOHANG);
                if (result == pid_ || (result < 0 && errno == ECHILD)) {
                    exited = true;
                    break;
                }
                if (result < 0 && errno != EINTR) break;
                usleep(10000);
            }
            if (!exited) {
                kill(pid_, SIGKILL);
                while (waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
            }
            pid_ = -1;
        }
        if (in_ >= 0)  { close(in_);  in_  = -1; }
        if (out_ >= 0) { close(out_); out_ = -1; }
        ioSource_.reset();
        timeSource_.reset();
        state_ = State::Dead;
        failPendingHandlers();
    }

    // 异步发送: handler 在主线程(IO 事件)被调用一次; 死亡时以 "" 调用
    void send(const std::string &line, Handler h) {
        if (pid_ <= 0 || in_ < 0) { if (h) h(""); return; }
        WLOG("engine send command=%c bytes=%zu queued=%zu state=%d\n",
             line.empty() ? '?' : line[0], line.size(), pending_.size(),
             static_cast<int>(state_));
        std::string out = line + "\n";
        ssize_t n = write(in_, out.data(), out.size());
        if (n < 0) {
            WLOG("write fail errno=%d → stop\n", errno);
            stop();
            if (h) h("");
            return;
        }
        pending_.push_back(std::move(h));
    }

private:
    enum class State { Dead, Starting, Ready };

    static uint64_t nowUsec() {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
    }

    void onReadable(int fd) {
        char tmp[8192];
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) {
            WLOG("engine EOF (n=%zd)\n", n);
            stop();
            return;
        }
        rbuf_.append(tmp, n);
        size_t pos;
        while ((pos = rbuf_.find('\n')) != std::string::npos) {
            std::string line = rbuf_.substr(0, pos);
            rbuf_.erase(0, pos + 1);
            onLine(line);
        }
    }

    void onLine(const std::string &line) {
        WLOG("engine line bytes=%zu prefix=%.12s queued=%zu state=%d\n",
             line.size(), line.c_str(), pending_.size(), static_cast<int>(state_));
        if (state_ == State::Starting) {
            if (line == "READY") {
                state_ = State::Ready;
                failCount_ = 0;
                timeSource_.reset();
                WLOG("engine READY\n");
                if (onReady_) onReady_();
            }
            return;   // READY 前的杂音行丢弃
        }
        if (state_ != State::Ready) return;
        if (!line.empty() && !pending_.empty()) {
            Handler h = std::move(pending_.front());
            pending_.pop_front();
            if (h) h(line);
        }
    }

    void onStartTimeout() {
        WLOG("engine start timeout → stop\n");
        stop();
    }

    void failPendingHandlers() {
        while (!pending_.empty()) {
            Handler h = std::move(pending_.front());
            pending_.pop_front();
            if (h) h("");
        }
    }

    EventLoop &loop_;
    pid_t pid_ = -1;
    int in_ = -1, out_ = -1;
    std::string rbuf_;
    State state_ = State::Dead;
    std::deque<Handler> pending_;
    std::unique_ptr<EventSourceIO> ioSource_;
    std::unique_ptr<EventSourceTime> timeSource_;
    std::function<void()> onReady_;
    int failCount_ = 0;
    bool gaveUp_ = false;
};

// ---------------------------------------------------------------- 主引擎类
class WeTypeEngine final : public InputMethodEngineV2 {
public:
    explicit WeTypeEngine(Instance *instance)
        : instance_(instance), eng_(instance->eventLoop()) {
        signal(SIGPIPE, SIG_IGN);
        std::string eng, dicts, work, qemu, sysroot;
        resolveDirs(eng, dicts, work, qemu, sysroot);
        WLOG("async addon init: eng=%s\n", eng.c_str());
        // Start lazily on the first pinyin key. The Android engine has a
        // background warmup path that can crash after sitting idle.
    }
    ~WeTypeEngine() override { eng_.stop(); }

    void keyEvent(const InputMethodEntry &, KeyEvent &event) override;

    void reset(const InputMethodEntry &entry, InputContextEvent &event) override {
        auto *ic = event.inputContext();
        if (!ic) return;
        bool had = !buf_.empty() || !cands_.empty();
        ++revision_;
        buf_.clear();
        cands_.clear();
        covers_.clear();
        candidatesCurrent_ = false;
        cancelPendingCandidates();
        windowStart_ = 0;
        selected_ = 0;
        expandedGrid_ = false;
        recoveryTried_ = false;
        eng_.send("SAVE", nullptr);
        eng_.send("C", nullptr);
        if (had) updateUI(*ic);
    }

    // 失焦/切换 IM: 彻底清理, 候选框随之消失
    void deactivate(const InputMethodEntry &entry, InputContextEvent &event) override {
        auto *ic = event.inputContext();
        if (!ic) return;
        // Fcitx calls deactivate before changing the active input method. If
        // Shift (or another IM toggle) is pressed mid-composition, preserve
        // the unfinished pinyin as literal text instead of dropping it.
        if (event.type() == EventType::InputContextSwitchInputMethod && !buf_.empty()) {
            WLOG("deactivate commits raw pinyin len=%zu\n", buf_.size());
            ic->commitString(buf_);
        }
        bool had = !buf_.empty() || !cands_.empty();
        ++revision_;
        buf_.clear();
        cands_.clear();
        covers_.clear();
        candidatesCurrent_ = false;
        cancelPendingCandidates();
        windowStart_ = 0;
        selected_ = 0;
        expandedGrid_ = false;
        recoveryTried_ = false;
        eng_.send("C", nullptr);
        if (had) updateUI(*ic);
    }

private:
    void updateUI(InputContext &ic) {
        auto &panel = ic.inputPanel();
        panel.reset();
        panel.setPreedit(pinyinPreedit(buf_));
        if (!cands_.empty()) {
            if (windowStart_ >= static_cast<int>(cands_.size())) {
                windowStart_ = 0;
                selected_ = 0;
            }
        }
        // Keep the candidate row from the first pinyin key. Kimpanel places the
        // panel below the cursor while it is only as tall as the preedit, then
        // flips it above once the row appears near the bottom of the screen.
        if (!buf_.empty() || !cands_.empty()) {
            const int start = windowStart_;
            const bool compact = !expandedGrid_ || cands_.empty();
            const int visiblePageSize = compact ? COMPACT_PAGE_SIZE : PAGE_SIZE;
            const int realCount = cands_.empty()
                                      ? 0
                                      : std::min(visiblePageSize,
                                                 static_cast<int>(cands_.size()) - start);
            if (realCount > 0 &&
                (selected_ < start || selected_ >= start + realCount)) {
                selected_ = start;
            }
            auto cl = std::make_unique<CommonCandidateList>();
            cl->setLayoutHint(CandidateLayoutHint::Horizontal);
            if (compact) {
                cl->setPageSize(COMPACT_PAGE_SIZE);
                for (int slot = 0; slot < COMPACT_PAGE_SIZE; ++slot) {
                    const int index = start + slot;
                    Text candidate;
                    if (slot < realCount) {
                        candidate.append(std::to_string(slot + 1) + " ");
                        candidate.append(cands_[index]);
                        cl->append<GridColumnCandidate>(std::move(candidate),
                            [this, index](InputContext *context) {
                                commitCandidate(context, index);
                            });
                    } else {
                        candidate.append(CANDIDATE_ROW_PAD);
                        cl->append<GridColumnCandidate>(std::move(candidate),
                            [](InputContext *) {});
                    }
                }
                // The API validates this index immediately against the list
                // size, so set it only after all compact candidates exist.
                cl->setGlobalCursorIndex(realCount > 0 ? selected_ - start : -1);
            } else {
                cl->setPageSize(GRID_COLUMNS);
                cl->setLabels(std::vector<std::string>(GRID_COLUMNS, ""));
                cl->setGlobalCursorIndex(-1);
                for (int col = 0; col < GRID_COLUMNS; ++col) {
                    Text column;
                    for (int row = 0; row < GRID_ROWS; ++row) {
                        const int index = start + row * GRID_COLUMNS + col;
                        if (index < start + realCount) {
                            const bool selected = index == selected_;
                            const auto label = std::to_string(index - start + 1) + " ";
                            const auto flag = selected ? TextFormatFlag::HighLight
                                                       : TextFormatFlag::NoFlag;
                            column.append(label, flag);
                            column.append(cands_[index], flag);
                        } else {
                            column.append(" ");
                        }
                        if (row + 1 < GRID_ROWS) column.append("\n");
                    }
                    const int selectedIndex = selected_;
                    cl->append<GridColumnCandidate>(std::move(column),
                        [this, selectedIndex](InputContext *context) {
                            commitCandidate(context, selectedIndex);
                        });
                }
            }
            panel.setCandidateList(std::move(cl));
        }
        ic.updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void commitText(InputContext *ic, const std::string &text) {
        WLOG("commit len=%zu revision=%llu\n", text.size(),
             static_cast<unsigned long long>(revision_));
        ic->commitString(text);
        ++revision_;
        buf_.clear();
        cands_.clear();
        covers_.clear();
        candidatesCurrent_ = false;
        cancelPendingCandidates();
        windowStart_ = 0;
        selected_ = 0;
        expandedGrid_ = false;
        recoveryTried_ = false;
        eng_.send("C", nullptr);          // 重建会话
        updateUI(*ic);
    }

    void commitCandidate(InputContext *ic, int index) {
        if (!candidatesCurrent_ || index < 0 || index >= static_cast<int>(cands_.size())) return;
        const int cover = index < static_cast<int>(covers_.size()) ? covers_[index] : 0;
        WLOG("commit candidate index=%d count=%zu cover=%d buffer_len=%zu revision=%llu\n", index,
             cands_.size(), cover, buf_.size(), static_cast<unsigned long long>(revision_));
        if (cover <= 0 || static_cast<size_t>(cover) >= buf_.size()) {
            eng_.send("S " + std::to_string(index), nullptr);
            commitText(ic, cands_[index]);
            return;
        }
        // The candidate covers only a prefix (e.g. 你好 of nihaoshijie): commit
        // it and keep composing the rest. The engine answers S with the
        // candidates for the remaining pinyin.
        ic->commitString(cands_[index]);
        ++revision_;
        buf_.erase(0, cover);
        cands_.clear();
        covers_.clear();
        candidatesCurrent_ = false;
        cancelPendingCandidates();
        windowStart_ = 0;
        selected_ = 0;
        expandedGrid_ = false;
        eng_.send("S " + std::to_string(index), candidateHandler());
        updateUI(*ic);
    }

    void ensureEngine() {
        if (eng_.alive() || eng_.gaveUp()) return;
        spansEnabled_ = false;
        if (!eng_.start()) return;
        // Ask for per-candidate input spans; older harnesses answer ERR and
        // keep whole-buffer selection.
        eng_.send("OPT spans", [this](const std::string &resp) {
            spansEnabled_ = resp == "OK";
            WLOG("candidate spans %s\n", spansEnabled_ ? "enabled" : "unavailable");
        });
    }

    void cancelPendingCandidates() {
        debounceSource_.reset();
        pendingKeys_.clear();
    }

    void scheduleCandidates(const std::string &keys) {
        pendingKeys_ += keys;
        debounceSource_.reset();
        if (pendingKeys_.size() >= CANDIDATE_DEBOUNCE_MAX_CHARS) {
            flushCandidates();
            return;
        }
        debounceSource_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, monotonicUsec() + CANDIDATE_DEBOUNCE_USEC, 0,
            [this](EventSourceTime *, uint64_t) {
                debounceSource_.reset();
                flushCandidates();
                return true;
            });
    }

    // Handles a CAND/EMPTY reply for the buffer as it is now; replies that
    // arrive after the buffer changed are ignored.
    EngineProc::Handler candidateHandler() {
        auto ref = icRef_;
        const auto expectedBuf = buf_;
        const auto expectedRevision = revision_;
        return [this, ref, expectedBuf, expectedRevision](const std::string &resp) {
            fcitx::InputContext *ic = ref.get();
            WLOG("response len=%zu prefix=%.4s expected_revision=%llu current_revision=%llu valid_ic=%d\n",
                 resp.size(), resp.c_str(), static_cast<unsigned long long>(expectedRevision),
                 static_cast<unsigned long long>(revision_), ic ? 1 : 0);
            if (!ic || expectedRevision != revision_ || expectedBuf != buf_) return;
            if (resp.empty()) {
                // Keep the last visible page while recovering; a transient
                // missing response must not collapse the candidate panel.
                updateUI(*ic);
                if (!buf_.empty() && !recoveryTried_) {
                    recoveryTried_ = true;
                    WLOG("engine response lost; restarting and replaying buffer_len=%zu revision=%llu\n",
                         buf_.size(), static_cast<unsigned long long>(revision_));
                    cancelPendingCandidates();
                    ensureEngine();
                    scheduleCandidates(buf_);
                }
                return;
            }
            if (resp != "EMPTY" && resp.rfind("CAND\t", 0) != 0) {
                // A partial selection the engine did not keep composing
                // (OK/ERR): rebuild the session from the remaining pinyin.
                WLOG("unexpected candidate reply prefix=%.4s; replaying buffer_len=%zu\n",
                     resp.c_str(), buf_.size());
                eng_.send("C", nullptr);
                scheduleCandidates(buf_);
                return;
            }
            cands_.clear();
            covers_.clear();
            if (resp.rfind("CAND\t", 0) == 0) {
                std::stringstream ss(resp.substr(5));
                std::string item;
                while (std::getline(ss, item, '\t')) {
                    int cover = 0;
                    if (spansEnabled_) {
                        const auto colon = item.find(':');
                        if (colon == std::string::npos) continue;
                        cover = std::atoi(item.c_str());
                        item.erase(0, colon + 1);
                    }
                    if (item.empty()) continue;
                    cands_.push_back(item);
                    covers_.push_back(cover);
                }
            }
            candidatesCurrent_ = true;
            WLOG("parsed candidates=%zu buffer_len=%zu\n", cands_.size(), buf_.size());
            windowStart_ = 0;
            selected_ = 0;
            updateUI(*ic);
        };
    }

    void flushCandidates() {
        if (buf_.empty() || pendingKeys_.empty()) return;
        std::string keys = std::move(pendingKeys_);
        pendingKeys_.clear();
        ensureEngine();
        WLOG("send batch chars=%zu buffer_len=%zu revision=%llu\n", keys.size(),
             buf_.size(), static_cast<unsigned long long>(revision_));
        eng_.send("B " + keys, candidateHandler());
    }

    Instance *instance_;
    EngineProc eng_;
    std::string buf_;
    std::string pendingKeys_;
    std::unique_ptr<EventSourceTime> debounceSource_;
    std::vector<std::string> cands_;
    std::vector<int> covers_;     // pinyin letters each candidate consumes (0 = unknown)
    bool spansEnabled_ = false;
    // Retained candidates remain visible during composition updates, but are
    // not selectable until a result for the current buffer arrives.
    bool candidatesCurrent_ = false;
    int windowStart_ = 0;
    int selected_ = 0;
    bool expandedGrid_ = false;
    bool recoveryTried_ = false;
    uint64_t revision_ = 0;
    TrackableObjectReference<InputContext> icRef_;

    void clearAll(InputContext *ic = nullptr) {
        ++revision_;
        buf_.clear();
        cands_.clear();
        covers_.clear();
        candidatesCurrent_ = false;
        cancelPendingCandidates();
        windowStart_ = 0;
        selected_ = 0;
        expandedGrid_ = false;
        recoveryTried_ = false;
        eng_.send("C", nullptr);
        if (ic) updateUI(*ic);
    }

};

void WeTypeEngine::keyEvent(const InputMethodEntry &, KeyEvent &event) {
    auto ic = event.inputContext();
    WLOG("keyEvent sym=%d ready=%d\n", (int)event.key().sym(), eng_.ready() ? 1 : 0);
    const auto sym = event.key().sym();
    bool handled = false;
    icRef_ = ic->watch();

    if (event.key().states().testAny(KeyStates{KeyState::Ctrl, KeyState::Alt,
                                               KeyState::Super, KeyState::Super2,
                                               KeyState::Meta, KeyState::Hyper,
                                               KeyState::Hyper2, KeyState::Mod5})) {
        return;
    }

    if (!event.isRelease()) {
        // Alphabet keys, including Shift/CapsLock symbols, become lowercase
        // pinyin. Ctrl/Alt/Super/Meta shortcuts were passed through above.
        if ((sym >= FcitxKey_a && sym <= FcitxKey_z) ||
            (sym >= FcitxKey_A && sym <= FcitxKey_Z)) {
            if (buf_.empty()) recoveryTried_ = false;
            const bool replayBuffer = !eng_.alive() && !buf_.empty();
            char c = (sym >= FcitxKey_a && sym <= FcitxKey_z)
                         ? static_cast<char>('a' + (sym - FcitxKey_a))
                         : static_cast<char>('a' + (sym - FcitxKey_A));
            ensureEngine();
            ++revision_;
            buf_ += c;
            candidatesCurrent_ = false;
            WLOG("typed alpha buffer_len=%zu pending_before=%zu revision=%llu\n",
                 buf_.size(), pendingKeys_.size(), static_cast<unsigned long long>(revision_));
            if (replayBuffer) cancelPendingCandidates();
            handled = true;
            // Refresh the preedit immediately while keeping the last
            // candidate page visible until the new engine response arrives.
            updateUI(*ic);
            scheduleCandidates(replayBuffer ? buf_ : std::string(1, c));
        }
        // The compact single row expands to the four-row grid on Down.
        else if (!buf_.empty() && candidatesCurrent_ && !cands_.empty() &&
                 (sym == FcitxKey_Left || sym == FcitxKey_Right ||
                  sym == FcitxKey_Up || sym == FcitxKey_Down)) {
            if (!expandedGrid_) {
                if (sym == FcitxKey_Down) {
                    expandedGrid_ = true;
                } else if (sym == FcitxKey_Left && selected_ > windowStart_) {
                    --selected_;
                } else if (sym == FcitxKey_Right &&
                           selected_ + 1 < std::min<int>(windowStart_ + COMPACT_PAGE_SIZE,
                                                       cands_.size())) {
                    ++selected_;
                }
                updateUI(*ic);
                handled = true;
            } else {
            const int start = windowStart_;
            const int count = std::min<int>(PAGE_SIZE,
                static_cast<int>(cands_.size()) - start);
            const int row = (selected_ - start) / GRID_COLUMNS;
            const int col = (selected_ - start) % GRID_COLUMNS;
            int next = selected_;
            if (sym == FcitxKey_Left && col > 0) next--;
            if (sym == FcitxKey_Right && col + 1 < GRID_COLUMNS && next + 1 < start + count) next++;
            if (sym == FcitxKey_Up) {
                if (row > 0) next -= GRID_COLUMNS;
                else if (start >= GRID_COLUMNS) {
                    windowStart_ -= GRID_COLUMNS;
                    next -= GRID_COLUMNS;
                }
            }
            if (sym == FcitxKey_Down) {
                if (next + GRID_COLUMNS < start + count) next += GRID_COLUMNS;
                else if (next + GRID_COLUMNS < static_cast<int>(cands_.size())) {
                    windowStart_ += GRID_COLUMNS;
                    next += GRID_COLUMNS;
                }
            }
            selected_ = next;
            updateUI(*ic);
            handled = true;
            }
        }
        // - / = / PgUp / PgDn : move between four-row candidate grids.
        else if (sym == FcitxKey_minus || sym == FcitxKey_Page_Up) {
            if (!buf_.empty() && windowStart_ > 0) {
                windowStart_ = std::max(0, windowStart_ - PAGE_SIZE);
                selected_ = windowStart_;
                updateUI(*ic);
                handled = true;
            }
        }
        else if (sym == FcitxKey_equal || sym == FcitxKey_plus ||
                 sym == FcitxKey_KP_Add || sym == FcitxKey_Page_Down) {
            if (!buf_.empty() && candidatesCurrent_ && !cands_.empty() && !expandedGrid_) {
                expandedGrid_ = true;
                updateUI(*ic);
                handled = true;
            } else if (!buf_.empty() && expandedGrid_ &&
                       candidatesCurrent_ && windowStart_ + PAGE_SIZE < (int)cands_.size()) {
                windowStart_ = std::min(windowStart_ + PAGE_SIZE,
                    ((static_cast<int>(cands_.size()) - 1) / GRID_COLUMNS) * GRID_COLUMNS);
                selected_ = windowStart_;
                updateUI(*ic);
                handled = true;
            }
        }
        // 逗号/句号: 组词中=首选顶字+标点; 空态透传
        else if (sym == FcitxKey_comma || sym == FcitxKey_period) {
            if (!buf_.empty()) {
                std::string punct = (sym == FcitxKey_comma) ? "," : ".";
                std::string text = !candidatesCurrent_ || cands_.empty() ? buf_ : cands_[selected_];
                if (candidatesCurrent_ && !cands_.empty())
                    eng_.send("S " + std::to_string(selected_), nullptr);
                WLOG("commit with punctuation text_len=%zu\n", text.size() + punct.size());
                ic->commitString(text + punct);
                ++revision_;
                buf_.clear();
                cands_.clear();
                covers_.clear();
                candidatesCurrent_ = false;
                cancelPendingCandidates();
                windowStart_ = 0;
                selected_ = 0;
                expandedGrid_ = false;
                eng_.send("C", nullptr);
                updateUI(*ic);
                handled = true;
            }
        }
        // 数字选词(全局序号 = 页首 + 数字)
        else if (sym >= FcitxKey_1 && sym <= FcitxKey_9) {
            int ordinal = static_cast<int>(sym - FcitxKey_1);
            int visibleCount = expandedGrid_ ? PAGE_SIZE : COMPACT_PAGE_SIZE;
            int idx = windowStart_ + ordinal;
            if (ordinal < visibleCount && candidatesCurrent_ && !cands_.empty() && idx < (int)cands_.size()) {
                commitCandidate(ic, idx);
                handled = true;
            }
        }
        // 空格: 上屏首选(或原文)
        else if (sym == FcitxKey_space) {
            if (!buf_.empty()) {
                if (candidatesCurrent_ && !cands_.empty()) commitCandidate(ic, selected_);   // 含部分选词与词库学习
                else commitText(ic, buf_);
                handled = true;
            }
        }
        // Enter commits the in-progress pinyin literally as Latin text.
        else if (sym == FcitxKey_Return) {
            if (!buf_.empty()) {
                WLOG("enter commits raw pinyin len=%zu\n", buf_.size());
                commitText(ic, buf_);
                handled = true;
            }
        }
        // 退格: C 重建 + 前缀重放
        else if (sym == FcitxKey_BackSpace) {
            if (!buf_.empty()) {
                buf_.pop_back();
                ++revision_;
                candidatesCurrent_ = false;
                cancelPendingCandidates();
                handled = true;
                eng_.send("C", nullptr);
                if (buf_.empty()) {
                    cands_.clear();
                    covers_.clear();
                    candidatesCurrent_ = false;
                    windowStart_ = 0;
                    selected_ = 0;
                    expandedGrid_ = false;
                    updateUI(*ic);
                } else {
                    scheduleCandidates(buf_);
                    updateUI(*ic);
                }
            }
        }
        // Esc: 清空
        else if (sym == FcitxKey_Escape) {
            if (!buf_.empty()) {
                ++revision_;
                buf_.clear();
                cands_.clear();
                covers_.clear();
                candidatesCurrent_ = false;
                cancelPendingCandidates();
                windowStart_ = 0;
                selected_ = 0;
                expandedGrid_ = false;
                eng_.send("C", nullptr);
                updateUI(*ic);
                handled = true;
            }
        }
    }

    if (handled) event.filterAndAccept();
}

class WeTypeEngineFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new WeTypeEngine(manager->instance());
    }
};

}  // namespace fcitx

FCITX_ADDON_FACTORY(fcitx::WeTypeEngineFactory);
