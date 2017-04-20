#include <stdio.h>
#include <stdint.h>
#include <sys/xattr.h>
#include <assert.h>

#define LC_BLOCK_SIZE 4096
#define LC_DIFF
#ifdef LC_DIFF
#define GETXATTR_SIZE LC_BLOCK_SIZE
#else
#define GETXATTR_SIZE sizeof(uint64_t)
#endif

struct pchange {
    uint16_t ch_len;
    uint8_t ch_type;
    char ch_path[0];
} __attribute__((packed));

int
main(int argc, char *argv[]) {
    char buf[GETXATTR_SIZE];
    struct pchange *pchange;
    size_t size, psize;

    if (argc == 2) {
        do {
            size = getxattr("/lcfs/lcfs", argv[1], buf, GETXATTR_SIZE);
            if (size == -1) {
                perror("getxattr");
                break;
            }
            if (size != LC_BLOCK_SIZE) {
                printf("Size of changes in layer %s is %ld\n",
                       argv[1], *(uint64_t *)buf);
                break;
            }
            psize = 0;
            while ((psize + sizeof(struct pchange)) < size) {
                pchange = (struct pchange *)&buf[psize];
                if (pchange->ch_len == 0) {
                    break;
                }
                printf("Type %d Len %d Path %s\n",
                       pchange->ch_type, pchange->ch_len, pchange->ch_path);
                psize += sizeof(struct pchange) + pchange->ch_len;
            }
        } while (psize);
    }
    return 0;
}
