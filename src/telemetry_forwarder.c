// telemetry_forwarder_v1.c

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SHM_PATH "/dev/shm/data"
#define SIZE 26

typedef struct {
    uint16_t magic;
    uint32_t seq;
    uint32_t timestamp;
    float speed;
    float batt;
    uint16_t fault;
    char version[6];
} Packet;

int main() {
    int fd = open(SHM_PATH, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    uint8_t *data = mmap(NULL, SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    Packet p;
    memcpy(&p, data, SIZE);

    if (p.magic != 0xDEAD) {
        printf("Invalid packet\n");
        return 0;
    }

    printf("Seq: %u\n", p.seq);
    printf("Speed: %.2f\n", p.speed);
    printf("Battery: %.2f\n", p.batt);
    printf("Version: %.6s\n", p.version);

    munmap(data, SIZE);
    close(fd);

    return 0;
}