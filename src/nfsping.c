#include "nfsping.h"

volatile sig_atomic_t quitting;

/* convert a timeval to microseconds */
unsigned long tv2us(struct timeval tv) {
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

/* convert a timeval to milliseconds */
unsigned long tv2ms(struct timeval tv) {
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* convert milliseconds to a timeval */
void ms2tv(struct timeval *tv, unsigned long ms) {
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
}

/* convert milliseconds to a timespec */
void ms2ts(struct timespec *ts, unsigned long ms) {
    ts->tv_sec = ms / 1000;
    ts->tv_nsec = (ms % 1000) * 1000000;
}

unsigned long ts2ms(struct timespec ts) {
    unsigned long ms = ts.tv_sec * 1000;
    ms += ts.tv_nsec / 1000000;
    return ms;
}

void int_handler(int sig) {
    quitting = 1;
}

void usage() {
    struct timeval timeout = NFS_TIMEOUT;
    struct timespec wait_time = NFS_WAIT;
    struct timespec sleep_time = NFS_SLEEP;

    printf("Usage: nfsping [options] [targets...]\n\
    -2    use NFS version 2\n\
    -A    show IP addresses\n\
    -c n  count of pings to send to target\n\
    -C n  same as -c, output parseable format\n\
    -d    reverse DNS lookups for targets\n\
    -i n  interval between targets (in ms, default %lu)\n\
    -l    loop forever\n\
    -m    use multiple target IP addresses if found\n\
    -M    use the portmapper (default no)\n\
    -p n  pause between pings to target (in ms, default %lu)\n\
    -P n  specify NFS port (default %i)\n\
    -q    quiet, only print summary\n\
    -t n  timeout (in ms, default %lu)\n\
    -T    use TCP (default UDP)\n",
    ts2ms(wait_time), ts2ms(sleep_time), NFS_PORT, tv2ms(timeout));

    exit(3);
}

void print_summary(targets_t targets) {
    targets_t *target = &targets;
    double loss;

    while (target) {
        loss = (target->sent - target->received) / (double)target->sent * 100;
        fprintf(stderr, "%s : xmt/rcv/%%loss = %u/%u/%.0f%%, min/avg/max = %.2f/%.2f/%.2f\n",
            target->name, target->sent, target->received, loss, target->min / 1000.0, target->avg / 1000.0, target->max / 1000.0);
        target = target->next;
    }
}

/* TODO target output spacing */
void print_verbose_summary(targets_t targets) {
    targets_t *target = &targets;
    results_t *current;

    while (target) {
        fprintf(stderr, "%s :", target->name);
        current = target->results;
        while (current) {
            if (current->us)
                fprintf(stderr, " %.2f", current->us / 1000.0);
            else
                fprintf(stderr, " -");
            current = current->next;
        }
        fprintf(stderr, "\n");
        target = target->next;
    }
}

int main(int argc, char **argv) {
    enum clnt_stat status;
    char *error;
    struct timeval timeout = NFS_TIMEOUT;
    struct timeval call_start, call_end;
    struct timespec sleep_time = NFS_SLEEP;
    struct timespec wait_time = NFS_WAIT;
    int sock = RPC_ANYSOCK;
    uint16_t port = htons(NFS_PORT);
    struct addrinfo hints, *addr;
    int getaddr;
    unsigned long us;
    double loss;
    targets_t *targets;
    targets_t *target;
    results_t *results;
    results_t *current;
    int ch;
    unsigned long count = 0;
    /* command-line options */
    int dns = 0, verbose = 0, loop = 0, ip = 0, quiet = 0, multiple = 0;
    /* default to NFS v3 */
    u_long version = 3;
    int first, index;

    /* listen for ctrl-c */
    quitting = 0;
    signal(SIGINT, int_handler);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    /* default to UDP */
    hints.ai_socktype = SOCK_DGRAM;

    /* no arguments passed */
    if (argc == 1)
        usage();

    while ((ch = getopt(argc, argv, "2Ac:C:dhi:lmMp:P:qt:T")) != -1) {
        switch(ch) {
            /* use NFS version 2 */
            case 2:
                version = 2;
                break;
            /* show IP addresses */
            case 'A':
                ip = 1;
                break;
            /* number of pings per target, parseable summary */
            case 'C':
                verbose = 1;
                /* fall through to regular count */
            /* number of pings per target */
            case 'c':
                count = strtoul(optarg, NULL, 10);
                /* TODO if count==0 */
                break;
            /* do reverse dns lookups for IP addresses */
            case 'd':
                dns = 1;
                break;
            /* interval between targets */
            case 'i':
                ms2ts(&wait_time, strtoul(optarg, NULL, 10));
                break;
            /* loop forever */
            case 'l':
                loop = 1;
                break;
            /* use multiple IP addresses if found */
            case 'm':
                multiple = 1;
                break;
            /* use the portmapper */
            case 'M':
                port = 0;
                break;
            /* time between pings to target */
            case 'p':
                ms2ts(&sleep_time, strtoul(optarg, NULL, 10));
                break;
            /* specify NFS port */
            case 'P':
                port = htons(strtoul(optarg, NULL, 10));
                break;
            /* quiet, only print summary */
            case 'q':
                quiet = 1;
                break;
            /* timeout */
            case 't':
                /* TODO check for zero */
                ms2tv(&timeout, strtoul(optarg, NULL, 10));
                break;
            /* use TCP */
            case 'T':
                hints.ai_socktype = SOCK_STREAM;
                break;
            /* timeout */
            case 'h':
            case '?':
            default:
                usage();
        }
    }

    /* mark the first non-option argument */
    first = optind;

    targets = calloc(1, sizeof(targets_t));
    target = targets;

    for (index = optind; index < argc; index++) {
        if (index > first) {
            target->next = calloc(1, sizeof(targets_t));
            target = target->next;
            target->next = NULL;
        }

        target->name = argv[index];

        if (verbose) {
            target->results = calloc(1, sizeof(results_t));
            target->current = target->results;
        }

        target->client_sock = calloc(1, sizeof(struct sockaddr_in));
        target->client_sock->sin_family = AF_INET;
        target->client_sock->sin_port = port;

        /* first try treating the hostname as an IP address */
        if (inet_pton(AF_INET, target->name, &((struct sockaddr_in *)target->client_sock)->sin_addr)) {
            /* if we have reverse lookups enabled */
            if (dns) {
                target->name = calloc(1, NI_MAXHOST);
                getaddr = getnameinfo((struct sockaddr *)target->client_sock, sizeof(struct sockaddr_in), target->name, NI_MAXHOST, NULL, 0, 0);
                if (getaddr > 0) { /* failure! */
                    fprintf(stderr, "%s: %s\n", target->name, gai_strerror(getaddr));
                    exit(EXIT_FAILURE);
                }
            }
        } else {
            /* if that fails, do a DNS lookup */
            /* we don't call freeaddrinfo because we keep a pointer to the sin_addr in the target */
            getaddr = getaddrinfo(argv[index], "nfs", &hints, &addr);
            if (getaddr == 0) { /* success! */
                /* loop through possibly multiple DNS responses */
                while (addr) {
                    target->client_sock->sin_addr = ((struct sockaddr_in *)addr->ai_addr)->sin_addr;

                    if (ip) {
                        target->name = calloc(1, INET_ADDRSTRLEN);
                        inet_ntop(AF_INET, &((struct sockaddr_in *)addr->ai_addr)->sin_addr, target->name, INET_ADDRSTRLEN);
                    }

                    /* multiple results */
                    if (addr->ai_next) {
                        if (multiple) {
                            /* create the next target */
                            target->next = calloc(1, sizeof(targets_t));
                            target = target->next;
                            target->next = NULL;
                            target->name = argv[index];

                            if (verbose) {
                                target->results = calloc(1, sizeof(results_t));
                                target->current = target->results;
                            }

                            target->client_sock = calloc(1, sizeof(struct sockaddr_in));
                            target->client_sock->sin_family = AF_INET;
                            target->client_sock->sin_port = port;
                        } else {
                            /* we have to look up the IP address if we haven't already for the warning */
                            if (!ip) {
                                target->name = calloc(1, INET_ADDRSTRLEN);
                                inet_ntop(AF_INET, &((struct sockaddr_in *)addr->ai_addr)->sin_addr, target->name, INET_ADDRSTRLEN);
                            }
                            fprintf(stderr, "Multiple addresses found for %s, using %s\n", argv[index], target->name);
                            /* if we're not using the IP address again we can free it */
                            if (!ip) {
                                free(target->name);
                                target->name = argv[index];
                            }
                            break;
                        }
                    }
                    addr = addr->ai_next;
                }
            } else {
                fprintf(stderr, "%s: %s\n", target->name, gai_strerror(getaddr));
                exit(EXIT_FAILURE);
            }
        }
    }

    /* reset back to start of list */
    target = targets;

    /* loop through the targets and create the rpc client */
    /* TODO should we exit on failure or just skip to the next target? */
    while (target) {
        /* TCP */
        if (hints.ai_socktype == SOCK_STREAM) {
            target->client = clnttcp_create(target->client_sock, NFS_PROGRAM, version, &sock, 0, 0);
            if (target->client == NULL) {
                clnt_pcreateerror("clnttcp_create");
                exit(EXIT_FAILURE);
            }
        /* UDP */
        } else {
            target->client = clntudp_create(target->client_sock, NFS_PROGRAM, version, timeout, &sock);
            if (target->client == NULL) {
                clnt_pcreateerror("clntudp_create");
                exit(EXIT_FAILURE);
            }
        }
        target->client->cl_auth = authnone_create();
        target = target->next;
    }

    /* reset back to start of list */
    target = targets;

    while(1) {
        if (quitting) {
            break;
        }

        while (target) {
            gettimeofday(&call_start, NULL);
            status = clnt_call(target->client, NFSPROC_NULL, (xdrproc_t) xdr_void, NULL, (xdrproc_t) xdr_void, error, timeout);
            gettimeofday(&call_end, NULL);
            target->sent++;

            if (status == RPC_SUCCESS) {
                /* check if we're not looping */
                if (!count && !loop) {
                    printf("%s is alive\n", target->name);
                    exit(EXIT_SUCCESS);
                }
                target->received++;
                loss = (target->sent - target->received) / (double)target->sent * 100;

                us = tv2us(call_end) - tv2us(call_start);

                /* first result is a special case */
                if (target->received == 1) {
                    target->min = target->max = target->avg = us;
                } else {
                    if (verbose) {
                        target->current->next = calloc(1, sizeof(results_t));
                        target->current = target->current->next;
                    }
                    if (us < target->min) target->min = us;
                    if (us > target->max) target->max = us;
                    /* calculate the average time */
                    target->avg = (target->avg * (target->received - 1) + us) / target->received;
                }

                if (verbose)
                    target->current->us = us;

                if (!quiet)
                    printf("%s : [%u], %03.2f ms (%03.2f avg, %.0f%% loss)\n", target->name, target->sent - 1, us / 1000.0, target->avg / 1000.0, loss);
            } else {
                clnt_perror(target->client, "clnt_call");
                if (!count && !loop) {
                    printf("%s is dead\n", target->name);
                    exit(EXIT_FAILURE);
                }
                if (verbose && target->sent > 1) {
                    target->current->next = calloc(1, sizeof(results_t));
                    target->current = target->current->next;
                }
            }

            target = target->next;
            if (target)
                nanosleep(&wait_time, NULL);
        }

        /* reset back to start of list */
        /* do this at the end of the loop not the start so we can check if we're done or need to sleep */
        target = targets;

        if (count && target->sent >= count) {
            break;
        }

        nanosleep(&sleep_time, NULL);
    }
    fflush(stdout);
    /* these print to stderr */
    if (!quiet)
        fprintf(stderr, "\n");
    if (verbose)
        print_verbose_summary(*targets);
    else
        print_summary(*targets);
    exit(EXIT_SUCCESS);
}
