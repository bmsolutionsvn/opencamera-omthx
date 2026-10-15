/*
 * receiver-validation.cpp — a peer that TCP-connects to port 6400 must not be able to spin the
 * receiver thread at 100% CPU, nor to make ~Channel()/Sender::stop() hang while it spins.
 *
 * The defect (Astra, 2026-10-15): the parse loop did
 *
 *     auto* header = reinterpret_cast<FrameHeader*>(recvBuffer.data());
 *     if (header->version != 1) { ... }
 *     size_t totalFrameLen = FrameHeader::SIZE + header->dataLength;   // dataLength is int32!
 *     if (totalFrameLen > bufferPos) break;
 *     ...
 *     bufferPos -= totalFrameLen;
 *
 * dataLength comes straight off the wire and is SIGNED. -16 makes totalFrameLen 0, so the shift at
 * the bottom of the loop consumes ZERO bytes: the same 16 bytes are re-parsed forever, `running_`
 * and `connected_` are never re-read, and one connect() from any host on the LAN pins a core at
 * 100%. ~Channel() joins that thread, so the app cannot tear the stream down either — the user's
 * only recovery is killing the process (issue #9 territory).
 *
 * Every case below runs in its own forked child with a wall-clock deadline, because the failure
 * mode is a HANG: a child that does not exit inside the deadline is killed and scored RED. Without
 * the fork, a red run and an "it is still running" run are indistinguishable in the log.
 *
 * What each hostile case asserts:
 *   (a) the receiver does not spin — the channel stops reporting itself connected inside 1 s AND
 *       the process burns < 40% of one core across a >= 400 ms observation window;
 *   (b) the channel is closed with a NAMED error (CloseReason, not just connected_ == false);
 *   (c) ~Channel() returns inside 1 s even though the thread may be wedged (measured from a worker
 *       thread, so the measurement itself cannot be the thing that hangs);
 *   (d) well-formed control frames still parse — including one split across two segments and two
 *       frames arriving in a single write, which is what keeps the memmove path honest.
 *
 * The select() -> poll() half is gated separately: `high-fd-poll` squats descriptors until the
 * accepted socket lands at fd >= 1100 (> FD_SETSIZE), where FD_SET writes outside the stack-resident
 * fd_set. Under the fixed build that frame parses; under a select() build ASan kills the child with
 * a stack-buffer-overflow inside receiverLoop, which the parent scores red.
 *
 * Build + run on the host (no device, no JVM):  bash qa/build-host.sh
 */
#include "OMTChannel.h"
#include "OMTConstants.h"
#include "OMTProtocol.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/select.h>   // FD_SETSIZE only: the high-fd case asserts against the ceiling select() has
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace omt;
using clock_ = std::chrono::steady_clock;

namespace {

int g_fail = 0;

void check(bool cond, const std::string& what) {
    std::printf("    [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    fflush(stdout);
    if (!cond) ++g_fail;
}

long msSince(clock_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_::now() - t0).count();
}

double cpuMs() {
    struct timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ------------------------------------------------------------------ wire helpers

/// One control frame: the 16-byte header followed by `payload`. dataLength/metadataLength are
/// taken verbatim, which is the whole point — a hostile case sends a header that disagrees with
/// the bytes behind it, exactly as an attacker would.
std::vector<uint8_t> frame(uint8_t version, uint8_t frameType, int32_t dataLength,
                           uint16_t metadataLength, const std::string& payload = std::string()) {
    FrameHeader h{};
    h.version = version;
    h.frameType = frameType;
    h.timestamp = 0;
    h.metadataLength = metadataLength;
    h.dataLength = dataLength;
    std::vector<uint8_t> out(FrameHeader::SIZE + payload.size());
    std::memcpy(out.data(), &h, FrameHeader::SIZE);
    if (!payload.empty()) std::memcpy(out.data() + FrameHeader::SIZE, payload.data(), payload.size());
    return out;
}

std::vector<uint8_t> goodFrame(const std::string& xml) {
    // A conforming metadata record: null-terminated, dataLength counts the terminator, and
    // metadataLength agrees with it (this is what Channel::sendMetadataSync writes in the other
    // direction, and what libomtnet's receiver expects to read).
    return frame(1, static_cast<uint8_t>(FrameType::Metadata),
                 static_cast<int32_t>(xml.size() + 1), static_cast<uint16_t>(xml.size() + 1),
                 xml + '\0');
}

// ------------------------------------------------------------------ the peer

/// The viewer side: a listening socket on loopback, a client connection to it, and the accepted
/// descriptor handed to Channel. The peer never reads anything it is not told to, so bytes the
/// receiver does not consume stay exactly where they were.
struct Peer {
    int listenFd = -1;
    int accepted = -1;   ///< handed to Channel; do not close after ownership transfers
    int client = -1;     ///< the attacker's end
    unsigned short port = 0;
    std::vector<int> squatted;

    void raiseFdLimit() {
        struct rlimit lim{};
        if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {
            rlim_t want = 8192;
            if (lim.rlim_max != RLIM_INFINITY && want > lim.rlim_max) want = lim.rlim_max;
            lim.rlim_cur = want;
            setrlimit(RLIMIT_NOFILE, &lim);
        }
    }

    /// Open /dev/null until the next descriptor would be >= floor, so the sockets created after
    /// this call are numbered above FD_SETSIZE (1024).
    bool squatUntil(int floor) {
        raiseFdLimit();
        for (int i = 0; i < 20000; ++i) {
            int fd = ::open("/dev/null", O_RDONLY);
            if (fd < 0) return false;
            squatted.push_back(fd);
            if (fd >= floor) return true;
        }
        return false;
    }

    void start(int fdFloor = 0) {
        if (fdFloor && !squatUntil(fdFloor)) {
            std::printf("    [FAIL] could not raise the descriptor table past %d\n", fdFloor);
            ++g_fail;
            return;
        }
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(listenFd, (sockaddr*)&a, sizeof(a)) != 0) { std::perror("    bind"); ++g_fail; return; }
        socklen_t sl = sizeof(a);
        getsockname(listenFd, (sockaddr*)&a, &sl);
        port = ntohs(a.sin_port);
        ::listen(listenFd, 4);

        client = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in c{};
        c.sin_family = AF_INET;
        c.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        c.sin_port = htons(port);
        if (::connect(client, (sockaddr*)&c, sizeof(c)) != 0) { std::perror("    connect"); ++g_fail; return; }
        accepted = ::accept(listenFd, nullptr, nullptr);
        if (accepted < 0) { std::perror("    accept"); ++g_fail; return; }
    }

    /// Ownership of the accepted end moves to the Channel, which closes it.
    Channel* makeChannel() {
        sockaddr_in addr{};
        socklen_t al = sizeof(addr);
        getpeername(accepted, (sockaddr*)&addr, &al);
        int fd = accepted;
        accepted = -1;
        return new Channel(fd, addr, Constants::NETWORK_SEND_BUFFER, nullptr);
    }

    bool sendRaw(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        while (n) {
            ssize_t w = ::send(client, b, n, MSG_NOSIGNAL);
            if (w <= 0) {
                if (w < 0 && (errno == EINTR)) continue;
                return false;
            }
            b += w;
            n -= (size_t)w;
        }
        return true;
    }

    void stop() {
        for (int fd : squatted) ::close(fd);
        squatted.clear();
        if (client >= 0) { shutdown(client, SHUT_RDWR); close(client); client = -1; }
        if (accepted >= 0) { shutdown(accepted, SHUT_RDWR); close(accepted); accepted = -1; }
        if (listenFd >= 0) { close(listenFd); listenFd = -1; }
    }
};

// ------------------------------------------------------------------ observation

struct Observation {
    bool closed = false;
    long closeMs = -1;
    double cpuRatio = 0.0;
};

/// Ship the hostile bytes, then watch two things the spin cannot fake: the channel has to stop
/// claiming to be connected, and the process has to stay off the CPU across a window that is at
/// least `observeMs` long whether or not the close happened.
Observation observeAfterHostile(Peer& peer, const std::vector<uint8_t>& bytes, Channel* ch,
                                long closeBudgetMs = 1000, long observeMs = 400) {
    Observation o;
    const double cpu0 = cpuMs();
    const auto t0 = clock_::now();
    peer.sendRaw(bytes.data(), bytes.size());
    while (msSince(t0) < closeBudgetMs) {
        if (!ch->isConnected()) { o.closed = true; o.closeMs = msSince(t0); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    while (msSince(t0) < observeMs) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const double wall = (double)msSince(t0);
    o.cpuRatio = wall > 1.0 ? (cpuMs() - cpu0) / wall : 0.0;
    return o;
}

/// Destroy the channel from a worker thread so a wedged receiver makes the WORKER hang, not the
/// thread that is trying to report the hang. Join if it came back; detach if it did not (destroying
/// a joinable std::thread calls std::terminate, and this test has already died that way once).
bool deleteWithin(Channel*& ch, long budgetMs, long* outMs) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    const auto t0 = clock_::now();
    std::thread th([ch, done] { delete ch; done->store(true); });
    while (!done->load() && msSince(t0) < budgetMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    *outMs = msSince(t0);
    if (done->load()) { th.join(); return true; }
    th.detach();
    return false;
}

// ------------------------------------------------------------------ cases

/// A hostile header must be refused, named, and torn down — and must never be observed burning a
/// core while we look. `expectReason == nullptr` means the frame is legal and the channel must
/// STAY connected (the neighbouring values that must not be refused).
int hostileCase(int32_t dataLength, uint16_t metadataLength, uint8_t frameType, uint8_t version,
                const char* expectReason, size_t payloadBytes, const char* label) {
    const bool legal = (expectReason == nullptr);
    std::string payload(payloadBytes, 'A');
    Peer peer;
    peer.start();
    Channel* ch = peer.makeChannel();

    const Observation o = observeAfterHostile(
        peer, frame(version, frameType, dataLength, metadataLength, payload), ch);

    std::printf("      %s: closed=%s after %ld ms, cpu/core over the window = %.2f\n",
                label, o.closed ? "yes" : "NO", o.closeMs, o.cpuRatio);

    if (legal) {
        check(!o.closed, (std::string(label) + ": a conforming frame does not evict the channel").c_str());
    } else {
        check(o.closed, (std::string(label) + ": the receiver closed the channel inside 1 s "
                        "instead of re-parsing the same header forever").c_str());
        check(std::strcmp(ch->closeReasonName(), expectReason) == 0,
              (std::string(label) + ": closed with the named error '") + expectReason + "' (got '" +
                  ch->closeReasonName() + "')");
    }
    // (a) the spin assertion, both directions: a legal frame must not spin either.
    check(o.cpuRatio < 0.4,
          (std::string(label) + ": the receiver thread did not spin (< 0.40 cpu per wall-clock)").c_str());

    // (c) teardown must be prompt even if the thread is wedged — measured from a worker so a hang
    // is reported rather than become the run.
    long dtorMs = -1;
    const bool torn = deleteWithin(ch, 1000, &dtorMs);
    std::printf("      ~Channel returned %s after %ld ms\n", torn ? "normally" : "NOT", dtorMs);
    check(torn, (std::string(label) + ": ~Channel() returned within 1 s (a wedged receiver thread "
                 "is the 'cannot stop the stream' symptom)").c_str());

    peer.stop();
    return g_fail ? 1 : 0;
}

/// (d) a well-formed control frame still parses.
int case_wellformed() {
    Peer peer;
    peer.start();
    Channel* ch = peer.makeChannel();

    const Observation o = observeAfterHostile(peer, goodFrame(MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO), ch);
    bool subscribed = false;
    const auto t0 = clock_::now();
    while (msSince(t0) < 1000) {
        if (ch->isVideoSubscribed()) { subscribed = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("      subscribe parsed=%s, connected=%s, cpu/core=%.2f\n",
                subscribed ? "yes" : "NO", ch->isConnected() ? "yes" : "no", o.cpuRatio);
    check(subscribed, "wellformed: a conforming metadata frame still reaches processMetadata()");
    check(ch->isConnected(), "wellformed: the conforming frame did not evict the subscriber");
    check(o.cpuRatio < 0.4, "wellformed: the receiver did not spin");

    long dtorMs = -1;
    check(deleteWithin(ch, 1000, &dtorMs), "wellformed: ~Channel() returned within 1 s");
    peer.stop();
    return g_fail ? 1 : 0;
}

/// The header arrives in one segment and the payload in the next, then two whole frames arrive in
/// a single write. This is the path the validation could easily break (a `break` that is really a
/// lost-frame bug), so it is asserted in the same build as the hostile cases.
int case_split_and_pipelined() {
    Peer peer;
    peer.start();
    Channel* ch = peer.makeChannel();

    const std::string xml = MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO;
    auto f = goodFrame(xml);
    peer.sendRaw(f.data(), FrameHeader::SIZE);                       // header only
    std::this_thread::sleep_for(std::chrono::milliseconds(50));       // let it poll an empty frame
    peer.sendRaw(f.data() + FrameHeader::SIZE, f.size() - FrameHeader::SIZE);

    auto t0 = clock_::now();
    while (!ch->isVideoSubscribed() && msSince(t0) < 1000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(ch->isVideoSubscribed(), "split: a header/payload split across two segments still parses");

    // two complete frames in one write — exercises the memmove shift between records
    std::string a = MetadataConstants::CHANNEL_SUBSCRIBE_AUDIO;
    std::vector<uint8_t> batch = goodFrame(a);
    auto fb = goodFrame(MetadataConstants::TALLY_PROGRAM);
    batch.insert(batch.end(), fb.begin(), fb.end());
    peer.sendRaw(batch.data(), batch.size());

    t0 = clock_::now();
    while (msSince(t0) < 1000) {
        if (ch->isAudioSubscribed() && ch->isTallyProgram()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(ch->isAudioSubscribed() && ch->isTallyProgram(),
          "pipelined: two frames in one write both parse (the memmove shift stays aligned)");
    check(ch->isConnected(), "pipelined: conforming traffic never evicts the subscriber");

    long dtorMs = -1;
    check(deleteWithin(ch, 1000, &dtorMs), "pipelined: ~Channel() returned within 1 s");
    peer.stop();
    return g_fail ? 1 : 0;
}

/// The select() half of the finding: FD_SET on a descriptor >= FD_SETSIZE writes outside the
/// stack-resident fd_set. Squat descriptors until the accepted socket is numbered past it, then
/// prove a normal control frame still parses. A select() build dies here with an ASan
/// stack-buffer-overflow inside receiverLoop; poll() has no ceiling.
int case_high_fd() {
    Peer peer;
    peer.start(1100);
    std::printf("      accepted descriptor = %d (FD_SETSIZE = %d)\n", peer.accepted, FD_SETSIZE);
    check(peer.accepted >= FD_SETSIZE, "high-fd: the accepted socket really is above FD_SETSIZE");
    Channel* ch = peer.makeChannel();

    const Observation o = observeAfterHostile(peer, goodFrame(MetadataConstants::CHANNEL_SUBSCRIBE_METADATA), ch);
    bool subscribed = false;
    const auto t0 = clock_::now();
    while (msSince(t0) < 1000) {
        if (ch->isMetadataSubscribed()) { subscribed = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(subscribed && ch->isConnected(),
          "high-fd: a control frame on fd >= FD_SETSIZE still parses (select() cannot do this)");
    check(o.cpuRatio < 0.4, "high-fd: the receiver did not spin");

    long dtorMs = -1;
    check(deleteWithin(ch, 1000, &dtorMs), "high-fd: ~Channel() returned within 1 s");
    peer.stop();
    return g_fail ? 1 : 0;
}

/// A peer that pipelines frames and then goes quiet while teardown is in flight must not be able to
/// hold ~Channel() open: the parse loop re-reads running_/connected_ on every iteration, so the rest
/// of the batch is abandoned. The frames are quality suggestions and the listener parks in
/// onQualityChanged() unless the test hands it its ONE permit, so frame #2 is only ever reached if
/// the loop ignores the flags — and reaching it parks the thread forever, which is what turns the
/// missing check into a hang rather than a coin flip.
struct ParkingListener : IChannelListener {
    std::atomic<int> entered{0};
    std::atomic<int> permits{0};      // each permit lets exactly one callback through
    std::atomic<bool> abandon{false}; // lets the child exit if the case has already failed
    void onDisconnected(Channel*) override {}
    void onTallyChanged(Channel*) override {}
    void onQualityChanged(Channel*) override {
        ++entered;
        while (permits.load() == 0 && !abandon.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (abandon.load()) return;
        permits.fetch_sub(1);
    }
};

int case_running_flag_stops_parse() {
    Peer peer;
    peer.start();
    sockaddr_in addr{};
    socklen_t al = sizeof(addr);
    getpeername(peer.accepted, (sockaddr*)&addr, &al);
    // static, not a local: on the failure path the receiver thread is still parked in this
    // listener when the case function returns, and a stack object would be gone by then.
    static ParkingListener listener;
    int fd = peer.accepted;
    peer.accepted = -1;
    Channel* ch = new Channel(fd, addr, Constants::NETWORK_SEND_BUFFER, &listener);

    // 64 quality-suggestion records in one write. They ALTERNATE High/Low on purpose:
    // processMetadata() only calls the listener when the suggested quality actually changes, so a
    // batch of identical frames would make frames 2..64 no-ops and the case would pass with the
    // flag check deleted (it did, on a first draft).
    std::vector<uint8_t> batch;
    for (int i = 0; i < 64; ++i) {
        auto f = goodFrame(i % 2 ? "<OMTSettings Quality=\"Low\" />"
                                 : "<OMTSettings Quality=\"High\" />");
        batch.insert(batch.end(), f.begin(), f.end());
    }
    peer.sendRaw(batch.data(), batch.size());

    const auto t0 = clock_::now();
    while (listener.entered.load() == 0 && msSince(t0) < 1000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(listener.entered.load() >= 1, "running-flag: the receiver reached the quality callback");
    check(listener.entered.load() == 1,
          "running-flag: it is parked INSIDE the first callback (no permit issued yet)");

    // Tear down with the callback parked. ~Channel() sets running_/connected_ false, then
    // shutdown()s the socket, then joins — so the FIN the peer sees is external proof that
    // running_ == false is already published, and only THEN may the callback be released. That
    // ordering is what makes this a construction rather than a race.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([ch, done] { delete ch; done->store(true); });

    auto eofSeen = std::make_shared<std::atomic<bool>>(false);
    std::thread releaser([&] {
        char buf[256];
        const auto t = clock_::now();
        while (msSince(t) < 1000) {
            // Consume, do not MSG_PEEK: any unread bytes would make a peek report ">0" forever and
            // the EOF would never be seen.
            ssize_t r = ::recv(peer.client, buf, sizeof(buf), MSG_DONTWAIT);
            if (r == 0) { *eofSeen = true; break; }   // ~Channel() shut the socket down
            if (r < 0 && errno == ECONNRESET) { *eofSeen = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        listener.permits.store(1);
    });

    long dtorMs = -1;
    while (!done->load() && msSince(t0) < 4000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    dtorMs = msSince(t0);
    const bool torn = done->load();
    releaser.join();
    if (torn) worker.join(); else { listener.abandon.store(true); worker.detach(); }

    std::printf("      entered callback %d time(s); teardown FIN observed=%s; ~Channel returned %s after %ld ms\n",
                listener.entered.load(), eofSeen->load() ? "yes" : "NO",
                torn ? "normally" : "NOT", dtorMs);
    // The whole case is only meaningful if teardown really got that far while the callback was
    // parked; assert the precondition rather than trusting it.
    check(eofSeen->load(),
          "running-flag: ~Channel() reached its shutdown() while the callback was parked "
          "(otherwise the case measured nothing)");
    check(torn, "running-flag: ~Channel() returned within 4 s — the parse loop re-checks running_ "
                "each iteration instead of finishing the peer's batch");
    check(listener.entered.load() == 1,
          "running-flag: frames 2..64 were abandoned, not parsed, once the channel was dead");

    peer.stop();
    return g_fail ? 1 : 0;
}

// ------------------------------------------------------------------ runner

struct Case {
    const char* name;
    int (*fn)();
};

constexpr long kCaseTimeoutMs = 8000;

/// One case per child process. The defect being tested is a spin and the failure mode is a hang, so
/// the parent must be able to score a child that never returns: wait WNOHANG against a deadline and
/// SIGKILL past it. `timeout 90` around the whole binary cannot distinguish "still running" from
/// "correctly red", and the harness has already been fooled once by an absent binary scored as a
/// pass — same family of mistake.
int run_case(const Case& c) {
    std::printf("  == %s\n", c.name);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) { check(false, "fork() failed"); return 1; }
    if (pid == 0) {
        int rc = c.fn();
        fflush(stdout);
        _exit(rc ? 1 : 0);
    }
    int status = 0;
    const auto t0 = clock_::now();
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) { check(false, "waitpid failed"); return 1; }
        if (msSince(t0) > kCaseTimeoutMs) {
            kill(pid, SIGKILL);
            int st = 0;
            waitpid(pid, &st, 0);
            std::printf("    [FAIL] %s: TIMEOUT after %ld ms — the case never returned, which is "
                        "the hang this test exists to catch. Scored RED, not 'still running'.\n",
                        c.name, (long)kCaseTimeoutMs);
            fflush(stdout);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        std::printf("    PASS %s\n", c.name);
        return 0;
    }
    if (WIFSIGNALED(status)) {
        std::printf("    FAIL %s: child died on signal %d (an ASan report above the verdict is the "
                    "evidence)\n", c.name, WTERMSIG(status));
    } else {
        std::printf("    FAIL %s: %d assertion(s) red\n", c.name, WEXITSTATUS(status));
    }
    fflush(stdout);
    return 1;
}

const Case kCases[] = {
    // (d) the good path first: if these go red the validation is wrong, not the attacker.
    {"wellformed", case_wellformed},
    {"split-and-pipelined", case_split_and_pipelined},
    {"high-fd-poll", case_high_fd},
    {"running-flag-stops-parse", case_running_flag_stops_parse},
    // The hostile set. -16 is the exact value that made totalFrameLen 0.
    {"hostile-datalen-minus16", [] { return hostileCase(-16, 0, 1, 1, "receiver: negative dataLength", 0, "datalen=-16"); }},
    {"hostile-datalen-minus1", [] { return hostileCase(-1, 0, 1, 1, "receiver: negative dataLength", 0, "datalen=-1"); }},
    // Neighbouring values: 0 and 65535 are LEGAL and must keep the channel up; 65536 and up are not.
    {"legal-datalen-0", [] { return hostileCase(0, 0, 1, 1, nullptr, 0, "datalen=0"); }},
    {"legal-datalen-65535", [] { return hostileCase(65535, 65535, 1, 1, nullptr, 65535, "datalen=65535"); }},
    {"hostile-datalen-65536", [] { return hostileCase(65536, 0, 1, 1, "receiver: oversized dataLength", 0, "datalen=65536"); }},
    {"hostile-datalen-int32max", [] { return hostileCase(0x7fffffff, 0, 1, 1, "receiver: oversized dataLength", 0, "datalen=INT32_MAX"); }},
    {"hostile-metalen-gt-datalen", [] { return hostileCase(8, 300, 1, 1, "receiver: metadataLength larger than dataLength", 8, "metadataLength=300"); }},
    {"hostile-frametype-video", [] { return hostileCase(0, 0, 2, 1, "receiver: unexpected peer frameType", 0, "frameType=Video"); }},
    {"hostile-version", [] { return hostileCase(0, 0, 1, 2, "receiver: bad frame header version", 0, "version=2"); }},
};

} // namespace

int main(int argc, char** argv) {
    // Line-buffer: the failure mode is a hang, and a killed process never flushes a block buffer.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    int bad = 0, run = 0;
    for (const Case& c : kCases) {
        if (filter && std::strcmp(filter, c.name) != 0) continue;
        ++run;
        bad += run_case(c);
    }
    if (run == 0) {
        std::printf("receiver-validation: NO CASE MATCHED '%s' — refusing to score an empty run\n",
                    filter ? filter : "");
        return 2;
    }
    std::printf("\nreceiver-validation: %s (%d/%d cases red)\n", bad ? "FAILURES" : "ALL PASS", bad, run);
    return bad ? 1 : 0;
}
