#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: diag_peer <ip> <port>\n");
        return 1;
    }
    const char *ip_str = argv[1];
    int port = atoi(argv[2]);

    printf("Connecting to %s:%d...\n", ip_str, port);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, ip_str, &sa.sin_addr);

    struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    printf("Connected! Sending standard BitTorrent handshake...\n");

    uint8_t info_hash[20] = {0x9B, 0x71, 0xDA, 0x18, 0x83, 0x52, 0x60, 0x09, 0xA2, 0xBE,
                             0xDE, 0xAA, 0x1A, 0x85, 0xC1, 0x00, 0x7D, 0x31, 0x3B, 0xD1};
    uint8_t peer_id[20] = "-TS0001-123456789012";

    uint8_t hs[68];
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);
    hs[25] = 0x10; // BEP 10
    hs[27] = 0x05; // BEP 5 + 6
    memcpy(hs + 28, info_hash, 20);
    memcpy(hs + 48, peer_id, 20);

    ssize_t s = send(sock, hs, 68, 0);
    printf("Sent %zd bytes. Waiting for response (timeout 4s)...\n", s);

    uint8_t rx[1024];
    ssize_t r = recv(sock, rx, sizeof(rx), 0);
    if (r < 0) {
        perror("recv error");
    } else if (r == 0) {
        printf("Connection closed by peer (EOF / RST)\n");
    } else {
        printf("Received %zd bytes!\n", r);
        for (ssize_t i = 0; i < r && i < 128; i++) {
            printf("%02X ", rx[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        printf("\n");
    }

    close(sock);
    return 0;
}
