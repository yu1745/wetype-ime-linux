// fcitx5 微信输入法 addon — 异步事件驱动版 (2026-09-10)
//
// 架构: fcitx5 主线程 IO 事件驱动, 全程零阻塞:
//   keyEvent → 写 "B c" 到引擎 stdin(内核缓冲, 不等) + 本地即时回显 buf_
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
#include <fcitx/inputcontextproperty.h>
#include <fcitx/surroundingtext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/candidatelist.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/trackableobject.h>

#include <algorithm>
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
#include <stdexcept>
#include <vector>
#include <sstream>
#include <unordered_set>

#include "glossary.h"
#include "punctuation.h"

namespace fcitx {

static constexpr int FALLBACK_PAGE_SIZE = 5;   // 取不到全局设置时的每页候选数
static constexpr uint64_t ENGINE_RESTART_DELAY_USEC = 200000;

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
    // QEMU 随安装一起提供，缺失时回落到系统包；bionic 运行时只随安装提供
    qemu = getenv("QEMU_AARCH64") ? getenv("QEMU_AARCH64") : "";
    if (qemu.empty())
        qemu = ::access((eng + "/qemu-aarch64-static").c_str(), X_OK) == 0
                   ? eng + "/qemu-aarch64-static" : "qemu-aarch64-static";
    const char *sr = getenv("WETYPE_SYSROOT");
    sysroot = sr && *sr ? sr : eng + "/sysroot";
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
    void setOnExit(std::function<void()> cb) { onExit_ = std::move(cb); }

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
            std::string libPath = eng + "/lib";
#if defined(__aarch64__)
            // Native bionic libc/liblog/libc++ come from the bundled sysroot.
            libPath += ":" + sysroot + "/system/lib64";
#endif
            setenv("LD_LIBRARY_PATH", libPath.c_str(), 1);
            setenv("WETYPE_DICT_DIR", dicts.c_str(), 1);
            setenv("WETYPE_ASSET_DIR", dicts.c_str(), 1);
            setenv("WETYPE_WORK_DIR", work.c_str(), 1);
#if defined(__aarch64__)
            // ARM64 host: exec the harness directly. Its ELF interpreter is set at
            // install time to the bundled sysroot/system/bin/linker64 (patchelf).
            execl((eng + "/wetype-harness").c_str(), (eng + "/wetype-harness").c_str(),
                  (eng + "/lib/libwxhld_jni.so").c_str(), "--daemon", (char *)nullptr);
#else
            execlp(qemu.c_str(), qemu.c_str(), "-L", sysroot.c_str(),
                   (eng + "/wetype-harness").c_str(),
                   (eng + "/lib/libwxhld_jni.so").c_str(), "--daemon", (char *)nullptr);
#endif
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
            die();
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
            die();
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
        die();
    }

    // 引擎意外退出: 清理后通知上层重启(主动 stop() 不通知)
    void die() {
        stop();
        if (onExit_) onExit_();
    }

    // Handlers may restart the engine and queue new commands; those belong to
    // the new process and must not be failed here.
    void failPendingHandlers() {
        std::deque<Handler> failed;
        failed.swap(pending_);
        for (auto &h : failed)
            if (h) h("");
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
    std::function<void()> onExit_;
    int failCount_ = 0;
    bool gaveUp_ = false;
};

// Punctuation history belongs to one input context and is never shared by
// Fcitx's ShareInputState option (the property's default needCopy() is false).
struct PunctuationState : InputContextProperty {
    wetype::TextLanguage language = wetype::TextLanguage::Unknown;
};

// ---------------------------------------------------------------- 主引擎类
class WeTypeEngine final : public InputMethodEngineV2 {
public:
    explicit WeTypeEngine(Instance *instance)
        : instance_(instance), eng_(instance->eventLoop()) {
        signal(SIGPIPE, SIG_IGN);
        if (!instance_->inputContextManager().registerProperty("wetype-punctuation-state", &punctuationFactory_)) {
            throw std::runtime_error("cannot register WeType punctuation state");
        }
        surroundingWatcher_ = instance_->watchEvent(
            EventType::InputContextSurroundingTextUpdated, EventWatcherPhase::Default,
            [this](Event &event) {
                auto *ic = static_cast<InputContextEvent &>(event).inputContext();
                if (!ic) return;
                const auto &surrounding = ic->surroundingText();
                auto *state = ic->propertyFor(&punctuationFactory_);
                state->language = wetype::TextLanguage::Unknown;
                if (surrounding.isValid() && surrounding.cursor() > 0) {
                    const auto &text = surrounding.text();
                    const auto bytes = utf8::ncharByteLength(text.begin(), surrounding.cursor());
                    state->language = wetype::endingLanguage(std::string_view(text.data(), bytes));
                }
            });
        const char *xdg = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        const std::string dataHome = xdg && *xdg ? xdg :
            std::string(home ? home : "/root") + "/.local/share";
        const std::size_t glossCount = glossary_.load(dataHome + "/wetype-ime/glossary-en.tsv");
        if (glossCount) WLOG("glossary loaded: %zu entries\n", glossCount);
        std::string eng, dicts, work, qemu, sysroot;
        resolveDirs(eng, dicts, work, qemu, sysroot);
        WLOG("async addon init: eng=%s\n", eng.c_str());
        // Start eagerly so the first key never waits for the ~1.5 s engine
        // startup, and bring the engine back as soon as it exits.
        eng_.setOnExit([this] { scheduleRestart(); });
        ensureEngine();
    }
    ~WeTypeEngine() override {
        cancelPending();
        restartSource_.reset();
        eng_.stop();
    }

    void keyEvent(const InputMethodEntry &, KeyEvent &event) override { processKey(event); }

    void reset(const InputMethodEntry &entry, InputContextEvent &event) override {
        auto *ic = event.inputContext();
        if (!ic) return;
        bool had = !buf_.empty() || !cands_.empty();
        eng_.send("SAVE", nullptr);
        clearAll();
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
            const auto raw = pendingRaw();
            rememberLanguage(ic, raw);
            ic->commitString(raw);
        } else if (event.type() == EventType::InputContextSwitchInputMethod) {
            ic->propertyFor(&punctuationFactory_)->language = wetype::TextLanguage::Unknown;
        }
        bool had = !buf_.empty() || !cands_.empty();
        clearAll();
        if (had) updateUI(*ic);
    }

private:
    void processKey(KeyEvent &event);

    // 每页候选数跟随 fcitx5 全局设置"候选词数量"(1-10), 取不到时回退 5。
    // 翻页步进、面板显示与数字选词都以它为准, 保证每个可见候选都能选中。
    int pageSize() const {
        const int n = instance_->globalConfig().defaultPageSize();
        return (n >= 1 && n <= 10) ? n : FALLBACK_PAGE_SIZE;
    }

    // Global page size can change while composing. Keep both indices valid
    // and align the page before rendering or handling another selection key.
    bool normalizePage() {
        const int oldStart = windowStart_;
        const int oldSelected = selected_;
        const int count = static_cast<int>(cands_.size());
        const int page = pageSize();
        if (count == 0) {
            windowStart_ = selected_ = 0;
        } else {
            windowStart_ = std::clamp(windowStart_, 0, count - 1);
            windowStart_ = (windowStart_ / page) * page;
            selected_ = std::clamp(selected_, windowStart_,
                                  std::min(windowStart_ + page, count) - 1);
        }
        return oldStart != windowStart_ || oldSelected != selected_ ||
               (!cands_.empty() && renderedPageSize_ != page);
    }

    void updateUI(InputContext &ic) {
        normalizePage();
        renderedPageSize_ = pageSize();
        auto &panel = ic.inputPanel();
        panel.reset();
        // Frontends may commit preedit on focus-out; include all consumed text.
        if (pending_) panel.setPreedit(Text(pendingRaw()));
        else panel.setPreedit(pinyinPreedit(buf_));
        if (!cands_.empty()) {
            const int start = windowStart_;
            const int end = std::min<int>(start + pageSize(), cands_.size());
            const int pageCount = end - start;
            if (selected_ < start || selected_ >= end) selected_ = start;
            auto cl = std::make_unique<CommonCandidateList>();
            cl->setLayoutHint(CandidateLayoutHint::Horizontal);
            cl->setPageSize(pageCount);
            for (int index = start; index < end; ++index) {
                Text candidate;
                // 序号即选词键: 第 10 个显示为 0, 与 fcitx5 的选词习惯一致
                const int ordinal = index - start;
                candidate.append(std::string(1, ordinal == 9 ? '0'
                                        : static_cast<char>('1' + ordinal)) + " ");
                candidate.append(cands_[index]);
                if (const auto *gloss = glossary_.lookup(cands_[index])) {
                    candidate.append(" " + *gloss, TextFormatFlag::Italic);
                }
                cl->append<GridColumnCandidate>(std::move(candidate),
                    [this, index](InputContext *context) {
                        commitCandidate(context, index);
                    });
            }
            // The API validates this index immediately against the list
            // size, so set it only after all candidates exist.
            cl->setGlobalCursorIndex(selected_ - start);
            panel.setCandidateList(std::move(cl));
        }
        ic.updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void rememberLanguage(InputContext *ic, std::string_view text) {
        ic->propertyFor(&punctuationFactory_)->language = wetype::endingLanguage(text);
    }

    wetype::TextLanguage punctuationLanguage(InputContext *ic, std::string_view text = {}) {
        // Structured input fields must retain ASCII separators.
        if (ic->capabilityFlags().testAny(CapabilityFlags{
                CapabilityFlag::Password, CapabilityFlag::Email, CapabilityFlag::Url,
                CapabilityFlag::Digit, CapabilityFlag::Number, CapabilityFlag::Dialable})) {
            return wetype::TextLanguage::Latin;
        }
        return text.empty() ? ic->propertyFor(&punctuationFactory_)->language
                            : wetype::endingLanguage(text);
    }

    void commitText(InputContext *ic, const std::string &text) {
        WLOG("commit len=%zu revision=%llu\n", text.size(),
             static_cast<unsigned long long>(revision_));
        rememberLanguage(ic, text);
        ic->commitString(text);
        ++revision_;
        buf_.clear();
        cands_.clear();
        covers_.clear();
        candidatesCurrent_ = false;
        windowStart_ = 0;
        selected_ = 0;
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
        rememberLanguage(ic, cands_[index]);
        ic->commitString(cands_[index]);
        ++revision_;
        buf_.erase(0, cover);
        cands_.clear();
        covers_.clear();
        candidatesCurrent_ = false;
        windowStart_ = 0;
        selected_ = 0;
        eng_.send("S " + std::to_string(index), candidateHandler());
        updateUI(*ic);
    }

    void scheduleRestart() {
        if (restartSource_ || eng_.gaveUp()) return;
        restartSource_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + ENGINE_RESTART_DELAY_USEC, 0,
            [this](EventSourceTime *, uint64_t) {
                restartSource_.reset();
                if (eng_.alive()) return true;
                WLOG("restarting engine after exit buffer_len=%zu\n", buf_.size());
                ensureEngine();
                // The new session is empty: replay the unfinished pinyin.
                requestCandidates(buf_);
                return true;
            });
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

    // Handles a CAND/EMPTY reply for the buffer as it is now. A reply for an
    // earlier prefix of the pinyin still being typed is shown as a
    // non-selectable preview so fast typing keeps refreshing the panel; other
    // stale replies are ignored.
    EngineProc::Handler candidateHandler() {
        auto ref = icRef_;
        const auto expectedEpoch = epoch_;
        const auto expectedBuf = buf_;
        const auto expectedRevision = revision_;
        return [this, ref, expectedEpoch, expectedBuf, expectedRevision](const std::string &resp) {
            fcitx::InputContext *ic = ref.get();
            WLOG("response len=%zu prefix=%.4s expected_revision=%llu current_revision=%llu valid_ic=%d\n",
                 resp.size(), resp.c_str(), static_cast<unsigned long long>(expectedRevision),
                 static_cast<unsigned long long>(revision_), ic ? 1 : 0);
            if (!ic || ic != icRef_.get() || expectedEpoch != epoch_ || resp == "SKIP") return;   // SKIP: merged into a later request
            const bool current = expectedRevision == revision_ && expectedBuf == buf_;
            const bool preview = !current && !expectedBuf.empty() &&
                                 buf_.size() > expectedBuf.size() &&
                                 buf_.compare(0, expectedBuf.size(), expectedBuf) == 0 &&
                                 resp.rfind("CAND\t", 0) == 0;
            if (!current && !preview) return;
            if (resp.empty()) {
                // Keep the last visible page while recovering; a transient
                // missing response must not collapse the candidate panel.
                updateUI(*ic);
                if (!buf_.empty() && !recoveryTried_) {
                    recoveryTried_ = true;
                    WLOG("engine response lost; restarting and replaying buffer_len=%zu revision=%llu\n",
                         buf_.size(), static_cast<unsigned long long>(revision_));
                    ensureEngine();
                    requestCandidates(buf_);
                }
                return;
            }
            if (resp != "EMPTY" && resp.rfind("CAND\t", 0) != 0) {
                // A partial selection the engine did not keep composing
                // (OK/ERR): rebuild the session from the remaining pinyin.
                WLOG("unexpected candidate reply prefix=%.4s; replaying buffer_len=%zu\n",
                     resp.c_str(), buf_.size());
                if (recoveryTried_) {
                    if (pending_) fallbackPending(ic);
                    return;
                }
                recoveryTried_ = true;
                eng_.send("C", nullptr);
                requestCandidates(buf_);
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
            candidatesCurrent_ = current;
            WLOG("parsed candidates=%zu buffer_len=%zu preview=%d\n", cands_.size(), buf_.size(),
                 preview ? 1 : 0);
            windowStart_ = 0;
            selected_ = 0;
            updateUI(*ic);
            if (current && pending_) {
                if (cands_.empty()) fallbackPending(ic);
                else {
                    disarmPending();
                    commitCandidate(ic, selected_);
                    drainQueued(ic);
                }
            }
        };
    }

    // Sends keys right away; the engine appends them to the current session.
    void requestCandidates(const std::string &keys) {
        if (buf_.empty() || keys.empty()) return;
        ensureEngine();
        WLOG("send keys chars=%zu buffer_len=%zu revision=%llu\n", keys.size(),
             buf_.size(), static_cast<unsigned long long>(revision_));
        eng_.send("B " + keys, candidateHandler());
    }

    struct QueuedKey {
        Key key, raw;
        bool release;
        int time;
    };
    bool pending_ = false;
    std::deque<QueuedKey> queued_;
    std::unique_ptr<EventSourceTime> pendingTimer_;
    bool draining_ = false;
    uint64_t epoch_ = 0;

    static std::string printableText(Key key) {
        const auto codepoint = Key::keySymToUnicode(key.sym());
        if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f)) return {};
        return Key::keySymToUTF8(key.sym());
    }

    static bool printable(Key key) { return !printableText(key).empty(); }

    std::string pendingRaw() const {
        std::string raw = buf_;
        if (pending_) raw += ' ';
        for (const auto &key : queued_) {
            if (!key.release) raw += printableText(key.key);
        }
        return raw;
    }

    void disarmPending() {
        pending_ = false;
        pendingTimer_.reset();
    }

    void cancelPending() {
        disarmPending();
        queued_.clear();
    }

    void fallbackPending(InputContext *ic) {
        const auto raw = pendingRaw();
        auto keys = std::move(queued_);
        cancelPending();
        commitText(ic, raw);
        for (const auto &key : keys) {
            if (key.release) ic->forwardKey(key.raw, true, key.time);
        }
    }

    void armPending(InputContext *ic) {
        pending_ = true;
        auto ref = ic->watch();
        const auto epoch = epoch_;
        // Finite wait covers SKIP, hung engines, and exhausted restarts.
        pendingTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 5000000, 0,
            [this, ref, epoch](EventSourceTime *, uint64_t) {
                if (auto *owner = ref.get(); owner && owner == icRef_.get() &&
                    epoch == epoch_ && pending_) fallbackPending(owner);
                return true;
            });
        updateUI(*ic);
        if ((candidatesCurrent_ && cands_.empty()) || eng_.gaveUp()) fallbackPending(ic);
    }

    void drainQueued(InputContext *ic) {
        if (draining_) return;
        draining_ = true;
        while (!pending_ && !queued_.empty() && icRef_.get() == ic) {
            const auto key = queued_.front();
            queued_.pop_front();
            KeyEvent event(ic, key.raw, key.release, key.time);
            event.setKey(key.key);
            processKey(event);
            if (!event.filtered()) ic->forwardKey(key.raw, key.release, key.time);
        }
        draining_ = false;
    }

    Instance *instance_;
    SimpleInputContextPropertyFactory<PunctuationState> punctuationFactory_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> surroundingWatcher_;
    wetype::Glossary glossary_;
    EngineProc eng_;
    std::string buf_;
    std::unique_ptr<EventSourceTime> restartSource_;
    std::vector<std::string> cands_;
    std::vector<int> covers_;     // pinyin letters each candidate consumes (0 = unknown)
    bool spansEnabled_ = false;
    // Retained candidates remain visible during composition updates, but are
    // not selectable until a result for the current buffer arrives.
    bool candidatesCurrent_ = false;
    int windowStart_ = 0;
    int selected_ = 0;
    int renderedPageSize_ = 0;
    bool recoveryTried_ = false;
    uint64_t revision_ = 0;
    TrackableObjectReference<InputContext> icRef_;

    void clearAll(InputContext *ic = nullptr) {
        cancelPending();
        ++epoch_;
        ++revision_;
        buf_.clear();
        cands_.clear();
        covers_.clear();
        candidatesCurrent_ = false;
        windowStart_ = 0;
        selected_ = 0;
        recoveryTried_ = false;
        eng_.send("C", nullptr);
        if (ic) updateUI(*ic);
    }

};

void WeTypeEngine::processKey(KeyEvent &event) {
    auto ic = event.inputContext();
    WLOG("keyEvent sym=%d ready=%d\n", (int)event.key().sym(), eng_.ready() ? 1 : 0);
    const auto sym = event.key().sym();
    bool handled = false;
    if (icRef_.get() != ic) clearAll();
    icRef_ = ic->watch();

    if (pending_) {
        const bool shortcut = event.key().states().testAny(KeyStates{
            KeyState::Ctrl, KeyState::Alt, KeyState::Super, KeyState::Super2,
            KeyState::Meta, KeyState::Hyper, KeyState::Hyper2, KeyState::Mod5});
        if (!event.isRelease() && !shortcut && sym == FcitxKey_Escape) {
            clearAll(ic);
            event.filterAndAccept();
            return;
        }
        if (!event.isRelease() && !shortcut && sym == FcitxKey_BackSpace) {
            // Delete the latest deferred press and any trailing release first.
            auto last = std::find_if(queued_.rbegin(), queued_.rend(),
                [](const QueuedKey &key) { return !key.release; });
            if (last != queued_.rend()) {
                auto press = std::prev(last.base());
                const auto raw = press->raw;
                auto next = queued_.erase(press);
                auto release = std::find_if(next, queued_.end(), [&raw](const QueuedKey &key) {
                    return key.release && key.raw.sym() == raw.sym();
                });
                if (release != queued_.end()) queued_.erase(release);
                updateUI(*ic);
                event.filterAndAccept();
                return;
            }
            // The pending Space remains an intent after editing the frozen
            // pinyin. Old replies are invalidated by the new revision.
            buf_.pop_back();
            ++revision_;
            candidatesCurrent_ = false;
            cands_.clear();
            covers_.clear();
            eng_.send("C", nullptr);
            if (buf_.empty()) fallbackPending(ic);
            else {
                requestCandidates(buf_);
                updateUI(*ic);
            }
            event.filterAndAccept();
            return;
        } else if (!event.isRelease() && (!printable(event.key()) || shortcut)) {
            // Navigation and shortcuts must not overtake consumed text.
            fallbackPending(ic);
            if (!shortcut && sym == FcitxKey_Return) event.filterAndAccept();
            return;
        } else {
            queued_.push_back({event.key(), event.rawKey(), event.isRelease(), event.time()});
            event.filterAndAccept();
            if (queued_.size() >= 256) fallbackPending(ic);
            else updateUI(*ic);
            return;
        }
    }

    if (event.key().states().testAny(KeyStates{KeyState::Ctrl, KeyState::Alt,
                                               KeyState::Super, KeyState::Super2,
                                               KeyState::Meta, KeyState::Hyper,
                                               KeyState::Hyper2, KeyState::Mod5})) {
        return;
    }

    if (!event.isRelease()) {
        if (normalizePage()) updateUI(*ic);
        // Digits passed through without surrounding-text support still make
        // decimal punctuation ASCII rather than inheriting a Chinese commit.
        if (buf_.empty() && sym >= FcitxKey_0 && sym <= FcitxKey_9) {
            ic->propertyFor(&punctuationFactory_)->language = wetype::TextLanguage::Latin;
        }
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
            WLOG("typed alpha buffer_len=%zu revision=%llu\n",
                 buf_.size(), static_cast<unsigned long long>(revision_));
            handled = true;
            // Refresh the preedit immediately while keeping the last
            // candidate page visible until the new engine response arrives.
            updateUI(*ic);
            requestCandidates(replayBuffer ? buf_ : std::string(1, c));
        }
        // 方向键: 左右在当前页候选内移动, 上/下 = 上一页/下一页
        else if (!buf_.empty() && candidatesCurrent_ && !cands_.empty() &&
                 (sym == FcitxKey_Left || sym == FcitxKey_Right ||
                  sym == FcitxKey_Up || sym == FcitxKey_Down)) {
            const int page = pageSize();
            if (sym == FcitxKey_Left && selected_ > windowStart_) {
                --selected_;
            } else if (sym == FcitxKey_Right &&
                       selected_ + 1 < std::min<int>(windowStart_ + page,
                                                     cands_.size())) {
                ++selected_;
            } else if (sym == FcitxKey_Up && windowStart_ > 0) {
                windowStart_ = std::max(0, windowStart_ - page);
                selected_ = windowStart_;
            } else if (sym == FcitxKey_Down &&
                       windowStart_ + page < (int)cands_.size()) {
                windowStart_ += page;
                selected_ = windowStart_;
            }
            // Composition navigation remains consumed at page boundaries.
            updateUI(*ic);
            handled = true;
        }
        // - / = / PgUp / PgDn : 按整页翻页。组词状态下这些键只用于翻页,
        // 即使已到边界也要吞掉, 否则 "-" "=" 会漏进目标文档 (issue #5)。
        else if (sym == FcitxKey_minus || sym == FcitxKey_Page_Up) {
            if (!buf_.empty()) {
                if (windowStart_ > 0) {
                    windowStart_ = std::max(0, windowStart_ - pageSize());
                    selected_ = windowStart_;
                    updateUI(*ic);
                }
                handled = true;
            }
        }
        else if (sym == FcitxKey_equal || sym == FcitxKey_plus ||
                 sym == FcitxKey_KP_Add || sym == FcitxKey_Page_Down) {
            if (!buf_.empty()) {
                if (candidatesCurrent_ &&
                    windowStart_ + pageSize() < (int)cands_.size()) {
                    windowStart_ += pageSize();
                    selected_ = windowStart_;
                    updateUI(*ic);
                }
                handled = true;
            }
        }
        // Composing: classify the text being committed, not a previous word.
        // Idle: convert only known CJK context; Latin/unknown remain passthrough.
        else if (sym == FcitxKey_comma || sym == FcitxKey_period) {
            const char key = sym == FcitxKey_comma ? ',' : '.';
            if (!buf_.empty()) {
                const std::string text = !candidatesCurrent_ || cands_.empty() ? buf_ : cands_[selected_];
                const auto punct = wetype::punctuation(key, punctuationLanguage(ic, text));
                if (candidatesCurrent_ && !cands_.empty())
                    eng_.send("S " + std::to_string(selected_), nullptr);
                WLOG("commit with punctuation text_len=%zu\n", text.size() + punct.size());
                // commitText sees a copy so clearing the buffer cannot invalidate it.
                commitText(ic, text + std::string(punct));
                handled = true;
            } else if (punctuationLanguage(ic) == wetype::TextLanguage::Cjk) {
                ic->commitString(std::string(wetype::punctuation(key, wetype::TextLanguage::Cjk)));
                handled = true;
            }
        }
        // 数字选词(全局序号 = 页首 + 数字, 0 选第 10 个)
        else if ((sym >= FcitxKey_1 && sym <= FcitxKey_9) ||
                 (sym == FcitxKey_0 && pageSize() >= 10)) {
            const int ordinal = (sym == FcitxKey_0) ? 9
                              : static_cast<int>(sym - FcitxKey_1);
            const int idx = windowStart_ + ordinal;
            if (ordinal < pageSize() && candidatesCurrent_ && !cands_.empty() &&
                idx < (int)cands_.size()) {
                commitCandidate(ic, idx);
                handled = true;
            }
        }
        // 空格: 上屏首选(或原文)
        else if (sym == FcitxKey_space) {
            if (!buf_.empty()) {
                if (candidatesCurrent_ && !cands_.empty()) commitCandidate(ic, selected_);   // 含部分选词与词库学习
                else armPending(ic);
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
                handled = true;
                eng_.send("C", nullptr);
                if (buf_.empty()) {
                    cands_.clear();
                    covers_.clear();
                    candidatesCurrent_ = false;
                    windowStart_ = 0;
                    selected_ = 0;
                    updateUI(*ic);
                } else {
                    requestCandidates(buf_);
                    updateUI(*ic);
                }
            }
        }
        // Esc: 清空
        else if (sym == FcitxKey_Escape) {
            if (!buf_.empty()) {
                clearAll(ic);
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
