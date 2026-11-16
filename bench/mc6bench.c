// SPDX-License-Identifier: GPL-2.0
/*
 * mc6bench — IPv6 UDP 멀티캐스트 수신 성능 측정 도구 (LinuxLOGK 측정 절차서 v1)
 *
 * 한 프로세스가 수신 namespace에서 실행되고, 송신 스레드만 setns()로 송신 namespace에 들어간다.
 * 그래서 송신 순번·시각과 측정 창 경계를 같은 메모리와 같은 시계(CLOCK_MONOTONIC)로 다룬다.
 *
 * 사용 예 (절차서 3절 구성: tx/rx namespace, veth0 <-> veth1):
 *   sudo ip netns exec rx ./mc6bench --workload wa --groups 1024 --rate 50000 \
 *        --rx-dev veth1 --tx-ns tx --tx-dev veth0 --warmup 10 --measure 60 \
 *        --meta phase=P2 --meta block=0 > result.jsonl
 *
 * 출력: 표준출력에 JSON 한 줄. 진행 메시지는 표준오류.
 * 종료 코드: 0 = 실행 완료(valid 여부는 JSON의 valid 필드), 2 = 인자 오류, 3 = 준비 실패.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <linux/sock_diag.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef IPV6_MULTICAST_ALL
#define IPV6_MULTICAST_ALL 29
#endif
#ifndef SO_MEMINFO
#define SO_MEMINFO 55
#endif

#define TOOL_VERSION "mc6bench 1.0"
#define PAYLOAD_LEN 128
#define MAGIC 0x4c4f474bU /* "LOGK" */
#define RX_VLEN 64
#define MAX_BATCH 64
#define MAX_META 32
#define LAT_BUCKETS 200000 /* 1µs 해상도, 200ms 이상은 마지막 칸 */

struct payload {
	uint32_t magic;
	uint32_t group;
	uint64_t seq;
	uint64_t send_ns;
	uint32_t len;
	uint32_t csum;
	uint8_t pad[PAYLOAD_LEN - 32];
} __attribute__((packed));
_Static_assert(sizeof(struct payload) == PAYLOAD_LEN, "payload size");

/* ---------- 설정 ---------- */
static struct {
	int wa;               /* 1 = W_A, 0 = W_B */
	int groups;
	double rate;          /* pps */
	int batch;
	double warmup_s, measure_s, settle_s, drain_s;
	const char *rx_dev, *tx_dev, *tx_ns;
	const char *group_base; /* 하위 16비트를 0..N-1 로 바꿈 */
	int port_base;
	int rcvbuf;           /* 0 = 기본값 유지 */
	uint64_t seed;
	const char *meta_k[MAX_META], *meta_v[MAX_META];
	int nmeta;
} cfg = {
	.wa = 1, .groups = 1, .rate = 10000, .batch = 32,
	.warmup_s = 10, .measure_s = 60, .settle_s = 2, .drain_s = 2,
	.rx_dev = "veth1", .tx_dev = "veth0", .tx_ns = "tx",
	.group_base = "ff15::1:0", .port_base = 5000, .seed = 1,
};

/* ---------- 공유 상태 ---------- */
static struct in6_addr *grp;          /* groups 개 */
static int rx_ifindex;
static uint64_t start_ns, t0_ns, t1_ns;

static atomic_int tx_ready, tx_failed_setup;
static atomic_uint_fast64_t seq_t0 = UINT64_MAX, seq_t1 = UINT64_MAX;
static atomic_uint_fast64_t tx_next_seq;     /* 다음에 보낼 순번 = 지금까지 성공 송신 수 */
static atomic_int tx_done;

/* 송신 통계 */
static uint64_t tx_ok_window, tx_calls, tx_partial, tx_err_calls, tx_late_batches;
static int tx_last_errno;
static double tx_thread_cpu_s_window;
static uint64_t tx_t0_ok, tx_t1_ok;

/* 수신 통계 */
static uint8_t *seen;                 /* 순번 비트맵 */
static uint64_t seen_cap;             /* 비트 수 */
static uint64_t rx_total, rx_in_window, rx_drain, rx_dup, rx_bad, rx_wrong_group, rx_oob;
static uint64_t rx_calls, rx_msgs_in_calls;
static uint64_t *lat_hist;            /* 송신 집합에 속한 패킷의 지연 */
static uint64_t lat_samples;

/* CPU 스냅샷 */
struct cpu_ticks { unsigned long long v[10]; };
static struct cpu_ticks cpu_t0, cpu_t1;
static uint64_t cpu_t0_read_ns, cpu_t1_read_ns;
static int cpu_ok;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
	struct timespec ts = { .tv_sec = t / 1000000000ull, .tv_nsec = t % 1000000000ull };
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static uint32_t csum_of(const struct payload *p)
{
	/* FNV-1a over magic, group, seq, send_ns, len */
	const uint8_t *b = (const uint8_t *)p;
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < offsetof(struct payload, csum); i++) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

static int read_cpu(struct cpu_ticks *c)
{
	FILE *f = fopen("/proc/stat", "r");
	if (!f)
		return -1;
	memset(c, 0, sizeof(*c));
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
		       &c->v[0], &c->v[1], &c->v[2], &c->v[3], &c->v[4],
		       &c->v[5], &c->v[6], &c->v[7], &c->v[8], &c->v[9]);
	fclose(f);
	return n >= 8 ? 0 : -1;
}

static void die_usage(const char *msg)
{
	if (msg)
		fprintf(stderr, "error: %s\n", msg);
	fprintf(stderr,
		"usage: mc6bench --workload wa|wb --groups N --rate PPS [options]\n"
		"  --batch B          sendmmsg 배치 크기 (기본 32, 최대 %d)\n"
		"  --warmup S         워밍업 초 (기본 10)\n"
		"  --measure S        측정 창 초 (기본 60)\n"
		"  --settle S         가입 후 안정화 초 (기본 2)\n"
		"  --drain S          측정 창 후 수신 대기 초 (기본 2)\n"
		"  --rx-dev IF        수신 인터페이스 (기본 veth1, 현재 namespace)\n"
		"  --tx-ns NAME|PATH  송신 namespace 이름 (기본 tx, /run/netns/NAME) 또는 경로\n"
		"  --tx-dev IF        송신 인터페이스 (기본 veth0)\n"
		"  --group-base ADDR  그룹 기준 주소 (기본 ff15::1:0, 하위 16비트를 0..N-1)\n"
		"  --port-base P      포트 기준 (기본 5000; W_A는 P+i, W_B는 P)\n"
		"  --rcvbuf BYTES     수신 소켓 SO_RCVBUF (기본: 커널 기본값)\n"
		"  --seed N           기록용 seed\n"
		"  --meta k=v         JSON에 그대로 넣을 메타데이터 (여러 번 가능)\n",
		MAX_BATCH);
	exit(2);
}

static void parse_args(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (die_usage("missing value"), (char *)0))
		if (!strcmp(a, "--workload")) {
			const char *w = NEXT();
			if (!strcmp(w, "wa")) cfg.wa = 1;
			else if (!strcmp(w, "wb")) cfg.wa = 0;
			else die_usage("--workload must be wa or wb");
		} else if (!strcmp(a, "--groups")) cfg.groups = atoi(NEXT());
		else if (!strcmp(a, "--rate")) cfg.rate = atof(NEXT());
		else if (!strcmp(a, "--batch")) cfg.batch = atoi(NEXT());
		else if (!strcmp(a, "--warmup")) cfg.warmup_s = atof(NEXT());
		else if (!strcmp(a, "--measure")) cfg.measure_s = atof(NEXT());
		else if (!strcmp(a, "--settle")) cfg.settle_s = atof(NEXT());
		else if (!strcmp(a, "--drain")) cfg.drain_s = atof(NEXT());
		else if (!strcmp(a, "--rx-dev")) cfg.rx_dev = NEXT();
		else if (!strcmp(a, "--tx-dev")) cfg.tx_dev = NEXT();
		else if (!strcmp(a, "--tx-ns")) cfg.tx_ns = NEXT();
		else if (!strcmp(a, "--group-base")) cfg.group_base = NEXT();
		else if (!strcmp(a, "--port-base")) cfg.port_base = atoi(NEXT());
		else if (!strcmp(a, "--rcvbuf")) cfg.rcvbuf = atoi(NEXT());
		else if (!strcmp(a, "--seed")) cfg.seed = strtoull(NEXT(), NULL, 0);
		else if (!strcmp(a, "--meta")) {
			char *kv = NEXT(), *eq = strchr(kv, '=');
			if (!eq || cfg.nmeta >= MAX_META) die_usage("--meta k=v");
			*eq = 0;
			cfg.meta_k[cfg.nmeta] = kv;
			cfg.meta_v[cfg.nmeta++] = eq + 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) die_usage(NULL);
		else die_usage("unknown option");
#undef NEXT
	}
	if (cfg.groups < 1 || cfg.groups > 65536) die_usage("--groups 1..65536");
	if (cfg.rate <= 0) die_usage("--rate > 0");
	if (cfg.batch < 1 || cfg.batch > MAX_BATCH) die_usage("--batch 1..64");
	if (cfg.measure_s <= 0 || cfg.warmup_s < 0) die_usage("bad durations");
}

/* ---------- 수신 준비 ---------- */
static int *rx_fd;
static int nrx;
static int join_fail, join_first_errno;
static long long so_meminfo_optmem = -1;

static int make_rx_socket(const struct in6_addr *bind_addr, int port)
{
	int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	int one = 1, zero = 0;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_ALL, &zero, sizeof(zero)) < 0) {
		fprintf(stderr, "IPV6_MULTICAST_ALL=0 failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (cfg.rcvbuf > 0)
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf, sizeof(cfg.rcvbuf));
	struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = htons(port),
				   .sin6_addr = *bind_addr };
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "bind port %d: %s\n", port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int join(int fd, const struct in6_addr *g)
{
	struct group_req gr = { .gr_interface = rx_ifindex };
	struct sockaddr_in6 *s = (struct sockaddr_in6 *)&gr.gr_group;
	s->sin6_family = AF_INET6;
	s->sin6_addr = *g;
	if (setsockopt(fd, IPPROTO_IPV6, MCAST_JOIN_GROUP, &gr, sizeof(gr)) < 0) {
		if (!join_fail++)
			join_first_errno = errno;
		return -1;
	}
	return 0;
}

static int setup_rx(void)
{
	rx_ifindex = if_nametoindex(cfg.rx_dev);
	if (!rx_ifindex) {
		fprintf(stderr, "rx dev %s not found\n", cfg.rx_dev);
		return -1;
	}
	nrx = cfg.wa ? cfg.groups : 1;
	rx_fd = calloc(nrx, sizeof(int));
	if (cfg.wa) {
		for (int i = 0; i < nrx; i++) {
			rx_fd[i] = make_rx_socket(&grp[i], cfg.port_base + i);
			if (rx_fd[i] < 0)
				return -1;
			join(rx_fd[i], &grp[i]);
		}
	} else {
		rx_fd[0] = make_rx_socket(&in6addr_any, cfg.port_base);
		if (rx_fd[0] < 0)
			return -1;
		for (int i = 0; i < cfg.groups; i++)
			join(rx_fd[0], &grp[i]);
		uint32_t mi[SK_MEMINFO_VARS] = { 0 };
		socklen_t l = sizeof(mi);
		if (getsockopt(rx_fd[0], SOL_SOCKET, SO_MEMINFO, mi, &l) == 0)
			so_meminfo_optmem = mi[SK_MEMINFO_OPTMEM];
	}
	return 0;
}

/* /proc/net/igmp6: 이 namespace의 인터페이스별 가입 그룹 (G_if) */
static int count_g_if(void)
{
	FILE *f = fopen("/proc/net/igmp6", "r");
	if (!f)
		return -1;
	char line[256], dev[IFNAMSIZ + 1];
	int idx, n = 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%d %16s", &idx, dev) == 2 && idx == rx_ifindex)
			n++;
	fclose(f);
	return n;
}

/* ---------- 송신 스레드 ---------- */
static void *tx_main(void *arg)
{
	(void)arg;
	char path[256];
	int nsfd = -1;
	if (strchr(cfg.tx_ns, '/')) { /* 경로를 직접 준 경우 */
		snprintf(path, sizeof(path), "%s", cfg.tx_ns);
		nsfd = open(path, O_RDONLY | O_CLOEXEC);
	} else {
		static const char *dirs[] = { "/run/netns", "/var/run/netns" };
		for (int d = 0; d < 2 && nsfd < 0; d++) {
			snprintf(path, sizeof(path), "%s/%s", dirs[d], cfg.tx_ns);
			nsfd = open(path, O_RDONLY | O_CLOEXEC);
		}
	}
	if (nsfd < 0 || setns(nsfd, CLONE_NEWNET) < 0) {
		fprintf(stderr, "setns %s: %s\n", path, strerror(errno));
		atomic_store(&tx_failed_setup, 1);
		atomic_store(&tx_ready, 1);
		return NULL;
	}
	close(nsfd);
	int ifx = if_nametoindex(cfg.tx_dev);
	int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int zero = 0, one = 1;
	if (!ifx || fd < 0 ||
	    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifx, sizeof(ifx)) < 0 ||
	    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &zero, sizeof(zero)) < 0 ||
	    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &one, sizeof(one)) < 0) {
		fprintf(stderr, "tx socket setup on %s failed: %s\n", cfg.tx_dev, strerror(errno));
		atomic_store(&tx_failed_setup, 1);
		atomic_store(&tx_ready, 1);
		return NULL;
	}
	atomic_store(&tx_ready, 1);

	static struct payload buf[MAX_BATCH];
	static struct sockaddr_in6 dst[MAX_BATCH];
	static struct iovec iov[MAX_BATCH];
	static struct mmsghdr msg[MAX_BATCH];
	const double ns_per_pkt = 1e9 / cfg.rate;
	uint64_t next = 0; /* 다음 순번 */
	struct timespec cpu_a = { 0 }, cpu_b = { 0 };
	int have_t0 = 0;

	sleep_until(start_ns);
	for (;;) {
		/* 배치가 시작될 시각: start + (next + batch) 개분 */
		uint64_t due = start_ns + (uint64_t)((double)(next + cfg.batch) * ns_per_pkt);
		uint64_t now = now_ns();
		if (now < due) {
			sleep_until(due);
			now = now_ns();
		} else if (now - due > 1000000ull) {
			tx_late_batches++; /* 1ms 이상 늦은 배치 */
		}
		if (now >= t1_ns) {
			atomic_store(&seq_t1, next);
			if (!have_t0) { atomic_store(&seq_t0, next); have_t0 = 1; }
			tx_t1_ok = next;
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_b);
			break;
		}
		if (!have_t0 && now >= t0_ns) {
			atomic_store(&seq_t0, next);
			tx_t0_ok = next;
			have_t0 = 1;
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_a);
		}
		int n = cfg.batch;
		for (int k = 0; k < n; k++) {
			uint64_t s = next + k;
			uint32_t g = (uint32_t)(s % (uint64_t)cfg.groups);
			struct payload *p = &buf[k];
			memset(p, 0, sizeof(*p));
			p->magic = MAGIC;
			p->group = g;
			p->seq = s;
			p->len = PAYLOAD_LEN;
			dst[k] = (struct sockaddr_in6){ .sin6_family = AF_INET6,
				.sin6_port = htons(cfg.wa ? cfg.port_base + (int)g : cfg.port_base),
				.sin6_addr = grp[g] };
			iov[k] = (struct iovec){ p, PAYLOAD_LEN };
			msg[k].msg_hdr = (struct msghdr){ .msg_name = &dst[k], .msg_namelen = sizeof(dst[k]),
							   .msg_iov = &iov[k], .msg_iovlen = 1 };
		}
		int off = 0;
		while (off < n) {
			uint64_t ts = now_ns(); /* 이 호출 직전 시각 = 미송신 항목의 송신 시각 */
			for (int k = off; k < n; k++) {
				buf[k].send_ns = ts;
				buf[k].csum = csum_of(&buf[k]);
			}
			int r = sendmmsg(fd, msg + off, n - off, 0);
			tx_calls++;
			if (r < 0) {
				tx_err_calls++;
				tx_last_errno = errno;
				if (errno == ENOBUFS || errno == EAGAIN || errno == EINTR)
					continue; /* 같은 순번으로 재시도, 순번 중복 없음 */
				fprintf(stderr, "sendmmsg: %s\n", strerror(errno));
				goto out;
			}
			if (off + r < n)
				tx_partial++;
			off += r;
			atomic_store(&tx_next_seq, next + off);
		}
		next += n;
	}
out:
	if (have_t0 && cpu_b.tv_sec == 0 && cpu_b.tv_nsec == 0)
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_b);
	tx_thread_cpu_s_window = (cpu_b.tv_sec - cpu_a.tv_sec) + (cpu_b.tv_nsec - cpu_a.tv_nsec) / 1e9;
	tx_ok_window = tx_t1_ok - tx_t0_ok;
	atomic_store(&tx_done, 1);
	close(fd);
	return NULL;
}

/* ---------- CPU 스냅샷 스레드 ---------- */
static void *meter_main(void *arg)
{
	(void)arg;
	sleep_until(t0_ns);
	int a = read_cpu(&cpu_t0);
	cpu_t0_read_ns = now_ns();
	sleep_until(t1_ns);
	int b = read_cpu(&cpu_t1);
	cpu_t1_read_ns = now_ns();
	cpu_ok = (a == 0 && b == 0);
	return NULL;
}

/* ---------- 수신 처리 ---------- */
static void handle(const struct payload *p, ssize_t len, int sock_idx, uint64_t rnow)
{
	rx_total++;
	if (len != PAYLOAD_LEN || p->magic != MAGIC || p->len != PAYLOAD_LEN ||
	    p->csum != csum_of(p) || p->group >= (uint32_t)cfg.groups ||
	    p->seq % (uint64_t)cfg.groups != p->group) {
		rx_bad++;
		return;
	}
	if (cfg.wa && (int)p->group != sock_idx) {
		rx_wrong_group++; /* 다른 그룹 소켓으로 전달됨 = 정확성 오류 */
		return;
	}
	if (p->seq >= seen_cap) {
		rx_oob++;
		return;
	}
	uint8_t bit = (uint8_t)(1u << (p->seq & 7));
	if (seen[p->seq >> 3] & bit) {
		rx_dup++;
		return;
	}
	seen[p->seq >> 3] |= bit;

	if (rnow >= t0_ns && rnow < t1_ns)
		rx_in_window++;
	else if (rnow >= t1_ns)
		rx_drain++;

	uint64_t s0 = atomic_load(&seq_t0), s1 = atomic_load(&seq_t1);
	if (p->seq >= s0 && p->seq < s1) {
		uint64_t us = rnow > p->send_ns ? (rnow - p->send_ns) / 1000 : 0;
		lat_hist[us < LAT_BUCKETS ? us : LAT_BUCKETS - 1]++;
		lat_samples++;
	}
}

static double pct_us(double q)
{
	if (!lat_samples)
		return -1;
	uint64_t want = (uint64_t)(q * (double)lat_samples);
	if (want >= lat_samples)
		want = lat_samples - 1;
	uint64_t acc = 0;
	for (int i = 0; i < LAT_BUCKETS; i++) {
		acc += lat_hist[i];
		if (acc > want)
			return i;
	}
	return LAT_BUCKETS - 1;
}

static void read_small(const char *path, char *out, size_t n)
{
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(out, (int)n, f))
		out[strcspn(out, "\n")] = 0;
	fclose(f);
}

static void json_str(const char *k, const char *v, int *first)
{
	printf("%s\"%s\":\"", *first ? "" : ",", k);
	for (const char *c = v; *c; c++) {
		if (*c == '"' || *c == '\\') putchar('\\');
		if ((unsigned char)*c >= 0x20) putchar(*c);
	}
	putchar('"');
	*first = 0;
}

#define J_NUM(k, fmt, v) printf(",\"%s\":" fmt, k, v)

int main(int argc, char **argv)
{
	parse_args(argc, argv);

	/* 그룹 주소 */
	struct in6_addr base;
	if (inet_pton(AF_INET6, cfg.group_base, &base) != 1 || base.s6_addr[0] != 0xff)
		die_usage("--group-base must be an IPv6 multicast address");
	grp = calloc(cfg.groups, sizeof(*grp));
	for (int i = 0; i < cfg.groups; i++) {
		grp[i] = base;
		grp[i].s6_addr[14] = (uint8_t)(i >> 8);
		grp[i].s6_addr[15] = (uint8_t)i;
	}

	/* 파일 디스크립터 한도 */
	struct rlimit rl;
	getrlimit(RLIMIT_NOFILE, &rl);
	if (rl.rlim_cur < (rlim_t)cfg.groups + 64) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_NOFILE, &rl);
		getrlimit(RLIMIT_NOFILE, &rl);
		if (rl.rlim_cur < (rlim_t)cfg.groups + 64) {
			fprintf(stderr, "RLIMIT_NOFILE %llu too small\n", (unsigned long long)rl.rlim_cur);
			return 3;
		}
	}

	if (setup_rx() < 0)
		return 3;
	int g_if = count_g_if();

	/* 순번 공간: (워밍업 + 측정) 동안 보낼 수 있는 최대 개수 + 여유 */
	seen_cap = (uint64_t)(cfg.rate * (cfg.warmup_s + cfg.measure_s + 1.0)) + 1024 * MAX_BATCH;
	seen = calloc(seen_cap / 8 + 1, 1);
	lat_hist = calloc(LAT_BUCKETS, sizeof(uint64_t));
	if (!seen || !lat_hist) {
		fprintf(stderr, "out of memory\n");
		return 3;
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	for (int i = 0; i < nrx; i++) {
		struct epoll_event ev = { .events = EPOLLIN, .data.u32 = (uint32_t)i };
		epoll_ctl(ep, EPOLL_CTL_ADD, rx_fd[i], &ev);
	}

	/* 시간표 */
	uint64_t setup_done = now_ns();
	start_ns = setup_done + (uint64_t)(cfg.settle_s * 1e9);
	t0_ns = start_ns + (uint64_t)(cfg.warmup_s * 1e9);
	t1_ns = t0_ns + (uint64_t)(cfg.measure_s * 1e9);
	uint64_t drain_end = t1_ns + (uint64_t)(cfg.drain_s * 1e9);

	pthread_t txt, mt;
	pthread_create(&txt, NULL, tx_main, NULL);
	while (!atomic_load(&tx_ready))
		usleep(1000);
	if (atomic_load(&tx_failed_setup))
		return 3;
	pthread_create(&mt, NULL, meter_main, NULL);
	fprintf(stderr, "%s: %s groups=%d g_if=%d rate=%.0f batch=%d join_fail=%d — start in %.1fs\n",
		TOOL_VERSION, cfg.wa ? "W_A" : "W_B", cfg.groups, g_if, cfg.rate, cfg.batch,
		join_fail, cfg.settle_s);

	/* 수신 루프 */
	static struct payload rbuf[RX_VLEN];
	struct iovec riov[RX_VLEN];
	struct mmsghdr rmsg[RX_VLEN];
	struct epoll_event evs[256];
	for (;;) {
		uint64_t now = now_ns();
		if (now >= drain_end && atomic_load(&tx_done))
			break;
		int ne = epoll_wait(ep, evs, 256, 10);
		for (int e = 0; e < ne; e++) {
			int si = (int)evs[e].data.u32;
			for (;;) {
				for (int k = 0; k < RX_VLEN; k++) {
					riov[k] = (struct iovec){ &rbuf[k], sizeof(rbuf[k]) };
					rmsg[k].msg_hdr = (struct msghdr){ .msg_iov = &riov[k], .msg_iovlen = 1 };
				}
				int r = recvmmsg(rx_fd[si], rmsg, RX_VLEN, MSG_DONTWAIT, NULL);
				if (r <= 0)
					break;
				uint64_t rnow = now_ns(); /* recvmmsg 반환 직후 시각 */
				rx_calls++;
				rx_msgs_in_calls += r;
				for (int k = 0; k < r; k++)
					handle(&rbuf[k], rmsg[k].msg_len, si, rnow);
				if (r < RX_VLEN)
					break;
			}
		}
	}
	pthread_join(txt, NULL);
	pthread_join(mt, NULL);

	/* 손실: 송신 집합 [seq_t0, seq_t1) 중 drain 끝까지 못 받은 것 */
	uint64_t s0 = atomic_load(&seq_t0), s1 = atomic_load(&seq_t1);
	uint64_t sent_set = (s1 > s0 && s1 != UINT64_MAX) ? s1 - s0 : 0, lost = 0;
	for (uint64_t s = s0; s < s1 && s < seen_cap; s++)
		if (!(seen[s >> 3] & (1u << (s & 7))))
			lost++;

	long clk = sysconf(_SC_CLK_TCK);
	unsigned long long d[10];
	for (int i = 0; i < 10; i++)
		d[i] = cpu_t1.v[i] - cpu_t0.v[i];
	unsigned long long kern = d[2] + d[5] + d[6]; /* system + irq + softirq */
	double kern_ns = (double)kern * 1e9 / (double)clk;
	double C = rx_in_window ? kern_ns / (double)rx_in_window : -1;
	double win_s = (t1_ns - t0_ns) / 1e9;
	double loss_pct = sent_set ? 100.0 * (double)lost / (double)sent_set : -1;
	double p99 = pct_us(0.99);

	/* 유효성 */
	const char *reason = NULL;
	if (atomic_load(&tx_failed_setup)) reason = "tx_setup";
	else if (!cpu_ok) reason = "cpu_read";
	else if (join_fail) reason = "join_fail";
	else if (rx_bad || rx_wrong_group || rx_dup) reason = "correctness";
	else if (!rx_in_window) reason = "no_rx";
	else if (sent_set && fabs((double)sent_set / win_s - cfg.rate) / cfg.rate > 0.01)
		reason = "tx_rate_off_by_1pct";

	struct utsname u;
	uname(&u);
	char boot_id[64], tainted[32], optmem[32];
	read_small("/proc/sys/kernel/random/boot_id", boot_id, sizeof(boot_id));
	read_small("/proc/sys/kernel/tainted", tainted, sizeof(tainted));
	read_small("/proc/sys/net/core/optmem_max", optmem, sizeof(optmem));

	int first = 1;
	putchar('{');
	json_str("tool", TOOL_VERSION, &first);
	for (int i = 0; i < cfg.nmeta; i++)
		json_str(cfg.meta_k[i], cfg.meta_v[i], &first);
	json_str("kernel", u.release, &first);
	json_str("boot_id", boot_id, &first);
	J_NUM("tainted", "%s", tainted[0] ? tainted : "null");
	J_NUM("vcpus", "%ld", sysconf(_SC_NPROCESSORS_ONLN));
	J_NUM("clk_tck", "%ld", clk);
	J_NUM("optmem_max", "%s", optmem[0] ? optmem : "null");
	printf(",\"workload\":\"%s\"", cfg.wa ? "wa" : "wb");
	J_NUM("g_app", "%d", cfg.groups);
	J_NUM("g_if", "%d", g_if);
	J_NUM("rate_pps", "%.0f", cfg.rate);
	J_NUM("batch", "%d", cfg.batch);
	J_NUM("payload", "%d", PAYLOAD_LEN);
	J_NUM("seed", "%" PRIu64, cfg.seed);
	J_NUM("rcvbuf", "%d", cfg.rcvbuf);
	J_NUM("warmup_s", "%.3f", cfg.warmup_s);
	J_NUM("measure_s", "%.3f", cfg.measure_s);
	J_NUM("t0_ns", "%" PRIu64, t0_ns);
	J_NUM("t1_ns", "%" PRIu64, t1_ns);
	J_NUM("cpu_t0_read_lag_us", "%.1f", (cpu_t0_read_ns - t0_ns) / 1e3);
	J_NUM("cpu_t1_read_lag_us", "%.1f", (cpu_t1_read_ns - t1_ns) / 1e3);
	printf(",\"cpu_ticks\":{\"user\":%llu,\"nice\":%llu,\"system\":%llu,\"idle\":%llu,"
	       "\"iowait\":%llu,\"irq\":%llu,\"softirq\":%llu,\"steal\":%llu}",
	       d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
	J_NUM("kernel_cpu_ticks", "%llu", kern);
	J_NUM("tx_ok_window", "%" PRIu64, tx_ok_window);
	J_NUM("tx_actual_pps", "%.1f", sent_set / win_s);
	J_NUM("tx_calls", "%" PRIu64, tx_calls);
	J_NUM("tx_partial", "%" PRIu64, tx_partial);
	J_NUM("tx_err_calls", "%" PRIu64, tx_err_calls);
	J_NUM("tx_last_errno", "%d", tx_last_errno);
	J_NUM("tx_late_batches", "%" PRIu64, tx_late_batches);
	J_NUM("tx_thread_cpu_pct", "%.1f", 100.0 * tx_thread_cpu_s_window / win_s);
	J_NUM("rx_total", "%" PRIu64, rx_total);
	J_NUM("rx_in_window", "%" PRIu64, rx_in_window);
	J_NUM("rx_drain", "%" PRIu64, rx_drain);
	J_NUM("rx_per_call", "%.2f", rx_calls ? (double)rx_msgs_in_calls / rx_calls : 0.0);
	J_NUM("sent_set", "%" PRIu64, sent_set);
	J_NUM("lost", "%" PRIu64, lost);
	J_NUM("dup", "%" PRIu64, rx_dup);
	J_NUM("bad", "%" PRIu64, rx_bad);
	J_NUM("wrong_group", "%" PRIu64, rx_wrong_group);
	J_NUM("out_of_range", "%" PRIu64, rx_oob);
	J_NUM("loss_pct", "%.5f", loss_pct);
	J_NUM("p50_us", "%.0f", pct_us(0.50));
	J_NUM("p99_us", "%.0f", p99);
	J_NUM("lat_samples", "%" PRIu64, lat_samples);
	J_NUM("join_fail", "%d", join_fail);
	J_NUM("join_first_errno", "%d", join_first_errno);
	J_NUM("so_meminfo_optmem", "%lld", so_meminfo_optmem);
	J_NUM("C_ns_per_pkt", "%.2f", C);
	int stable = loss_pct >= 0 && loss_pct <= 0.01 && p99 >= 0 && p99 <= 10000.0;
	printf(",\"stable\":%s", stable ? "true" : "false"); /* 손실 <=0.01%%, p99 <=10ms */
	printf(",\"valid\":%s", reason ? "false" : "true");
	if (reason)
		printf(",\"invalid_reason\":\"%s\"", reason);
	else
		printf(",\"invalid_reason\":null");
	printf("}\n");
	fflush(stdout);
	return 0;
}
