#define CCONTROL_TARGET (100 * 1000) // us
#define RATE_CHECK_INTERVAL 10000 // ms
#define DYNAMIC_PACKET_SIZE_ENABLED false
#define DYNAMIC_PACKET_SIZE_FACTOR 2

// POSIX uses lowercase sockaddr_storage; libutp expects the Windows name.
#ifndef SOCKADDR_STORAGE
#define SOCKADDR_STORAGE struct sockaddr_storage
#endif

#ifndef FORCEINLINE
#define FORCEINLINE inline
#endif

// This should return the global number of bytes sent, used for determining dynamic
// packet size based on rate. Current build is single-connection per socket and
// rate control is driven by the window/CC logic, so a stub is sufficient.
static inline uint64 UTP_GetGlobalUTPBytesSent(const struct sockaddr *remote, socklen_t remotelen) {
    (void)remote; (void)remotelen;
    return 0;
}

enum bandwidth_type_t {
	payload_bandwidth, connect_overhead,
	close_overhead, ack_overhead,
	header_overhead, retransmit_overhead
};

#ifdef WIN32
#define I64u "%I64u"
#else
#define I64u "%llu"
#endif
#ifdef WIN32
#define snprintf _snprintf
#endif

#define g_log_utp 0
#define g_log_utp_verbose 0
void utp_log(char const* fmt, ...)
{
	/*
	printf("[%u] ", UTP_GetMilliseconds());
	va_list vl;
	va_start(vl, fmt);
	vprintf(fmt, vl);
	va_end(vl);
	puts("");
	fflush(stdout);
	*/
};
