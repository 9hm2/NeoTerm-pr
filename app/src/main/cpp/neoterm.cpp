#include <dirent.h>
#include <fcntl.h>
#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>

#define __neoterm_no_return __attribute__((__noreturn__))

#define TERMUX_UNUSED(x) x __attribute__((__unused__))
#ifdef __APPLE__
# define LACKS_PTSNAME_R
#endif

static int throw_runtime_exception(JNIEnv *env, char const *message) {
    jclass exClass = env->FindClass("java/lang/RuntimeException");
    env->ThrowNew(exClass, message);
    return -1;
}

static int create_subprocess(JNIEnv *env,
                             char const *cmd,
                             char const *cwd,
                             char *const argv[],
                             char **envp,
                             int *pProcessId,
                             jint rows,
                             jint columns) {
    int ptm = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
    if (ptm < 0) return throw_runtime_exception(env, "Cannot open /dev/ptmx");

#ifdef LACKS_PTSNAME_R
    char* devname;
#else
    char devname[64];
#endif
    if (grantpt(ptm) || unlockpt(ptm) ||
        #ifdef LACKS_PTSNAME_R
        (devname = ptsname(ptm)) == NULL
        #else
        ptsname_r(ptm, devname, sizeof(devname))
#endif
            ) {
        return throw_runtime_exception(env, "Cannot grantpt()/unlockpt()/ptsname_r() on /dev/ptmx");
    }

    // Enable UTF-8 mode and disable flow control to prevent Ctrl+S from locking up the display.
    struct termios tios;
    tcgetattr(ptm, &tios);
    tios.c_iflag |= IUTF8;
    tios.c_iflag &= ~(IXON | IXOFF);
    tcsetattr(ptm, TCSANOW, &tios);

    /** Set initial winsize. */
    struct winsize sz = {.ws_row = static_cast<unsigned short>(rows), .ws_col = static_cast<unsigned short>(columns)};
    ioctl(ptm, TIOCSWINSZ, &sz);

    pid_t pid = fork();
    if (pid < 0) {
        return throw_runtime_exception(env, "Fork failed");
    } else if (pid > 0) {
        *pProcessId = (int) pid;
        return ptm;
    } else {
        // Clear signals which the Android java process may have blocked:
        sigset_t signals_to_unblock;
        sigfillset(&signals_to_unblock);
        sigprocmask(SIG_UNBLOCK, &signals_to_unblock, 0);

        close(ptm);
        setsid();

        int pts = open(devname, O_RDWR);
        if (pts < 0) exit(-1);

        dup2(pts, 0);
        dup2(pts, 1);
        dup2(pts, 2);

        DIR *self_dir = opendir("/proc/self/fd");
        if (self_dir != NULL) {
            int self_dir_fd = dirfd(self_dir);
            struct dirent *entry;
            while ((entry = readdir(self_dir)) != NULL) {
                int fd = atoi(entry->d_name);
                if (fd > 2 && fd != self_dir_fd) close(fd);
            }
            closedir(self_dir);
        }

        clearenv();
        if (envp) for (; *envp; ++envp) putenv(*envp);

        if (chdir(cwd) != 0) {
            char *error_message;
            // No need to free asprintf()-allocated memory since doing execvp() or exit() below.
            if (asprintf(&error_message, "chdir(\"%s\")", cwd) == -1)
                error_message = const_cast<char *>("chdir()");
            perror(error_message);
            fflush(stderr);
        }
        execvp(cmd, argv);
        // Show terminal output about failing exec() call:
        char *error_message;
        if (asprintf(&error_message, "exec(\"%s\")", cmd) == -1)
            error_message = const_cast<char *>("exec()");;
        perror(error_message);
        _exit(1);
    }
}

extern "C" JNIEXPORT jint JNICALL Java_io_neoterm_backend_JNI_createSubprocess(
        JNIEnv *env,
        jclass TERMUX_UNUSED(clazz),
        jstring cmd,
        jstring cwd,
        jobjectArray args,
        jobjectArray envVars,
        jintArray processIdArray,
        jint rows,
        jint columns) {
    jsize size = args ? env->GetArrayLength(args) : 0;
    char **argv = NULL;
    if (size > 0) {
        argv = (char **) malloc((size + 1) * sizeof(char *));
        if (!argv) return throw_runtime_exception(env, "Couldn't allocate argv array");
        for (int i = 0; i < size; ++i) {
            jstring arg_java_string = (jstring) env->GetObjectArrayElement(args, i);
            char const *arg_utf8 = env->GetStringUTFChars(arg_java_string, NULL);
            if (!arg_utf8)
                return throw_runtime_exception(env, "GetStringUTFChars() failed for argv");
            argv[i] = strdup(arg_utf8);
            env->ReleaseStringUTFChars(arg_java_string, arg_utf8);
        }
        argv[size] = NULL;
    }

    size = envVars ? env->GetArrayLength(envVars) : 0;
    char **envp = NULL;
    if (size > 0) {
        envp = (char **) malloc((size + 1) * sizeof(char *));
        if (!envp) return throw_runtime_exception(env, "malloc() for envp array failed");
        for (int i = 0; i < size; ++i) {
            jstring env_java_string = (jstring) env->GetObjectArrayElement(envVars, i);
            char const *env_utf8 = env->GetStringUTFChars(env_java_string, 0);
            if (!env_utf8)
                return throw_runtime_exception(env, "GetStringUTFChars() failed for env");
            envp[i] = strdup(env_utf8);
            env->ReleaseStringUTFChars(env_java_string, env_utf8);
        }
        envp[size] = NULL;
    }

    int procId = 0;
    char const *cmd_cwd = env->GetStringUTFChars(cwd, NULL);
    char const *cmd_utf8 = env->GetStringUTFChars(cmd, NULL);
    int ptm = create_subprocess(env, cmd_utf8, cmd_cwd, argv, envp, &procId, rows, columns);
    env->ReleaseStringUTFChars(cmd, cmd_utf8);
    env->ReleaseStringUTFChars(cmd, cmd_cwd);

    if (argv) {
        for (char **tmp = argv; *tmp; ++tmp) free(*tmp);
        free(argv);
    }
    if (envp) {
        for (char **tmp = envp; *tmp; ++tmp) free(*tmp);
        free(envp);
    }

    int *pProcId = (int *) env->GetPrimitiveArrayCritical(processIdArray, NULL);
    if (!pProcId)
        return throw_runtime_exception(env,
                                       "JNI call GetPrimitiveArrayCritical(processIdArray, &isCopy) failed");

    *pProcId = procId;
    env->ReleasePrimitiveArrayCritical(processIdArray, pProcId, 0);

    return ptm;
}

extern "C" JNIEXPORT void JNICALL
Java_io_neoterm_backend_JNI_setPtyWindowSize(JNIEnv *TERMUX_UNUSED(env),
                                              jclass TERMUX_UNUSED(clazz),
                                              jint fd, jint rows,
                                              jint cols) {
    struct winsize sz = {.ws_row = static_cast<unsigned short>(rows), .ws_col = static_cast<unsigned short>(cols)};
    ioctl(fd, TIOCSWINSZ, &sz);
}

extern "C" JNIEXPORT void JNICALL
Java_io_neoterm_backend_JNI_setPtyUTF8Mode(JNIEnv *TERMUX_UNUSED(env), jclass TERMUX_UNUSED(clazz),
                                            jint fd) {
    struct termios tios;
    tcgetattr(fd, &tios);
    if ((tios.c_iflag & IUTF8) == 0) {
        tios.c_iflag |= IUTF8;
        tcsetattr(fd, TCSANOW, &tios);
    }
}

extern "C" JNIEXPORT int JNICALL
Java_io_neoterm_backend_JNI_waitFor(JNIEnv *TERMUX_UNUSED(env), jclass TERMUX_UNUSED(clazz),
                                     jint pid) {
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return -WTERMSIG(status);
    } else {
        // Should never happen - waitpid(2) says "One of the first three macros will evaluate to a non-zero (true) value".
        return 0;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_io_neoterm_backend_JNI_close(JNIEnv *TERMUX_UNUSED(env), jclass TERMUX_UNUSED(clazz),
                                   jint fileDescriptor) {
    close(fileDescriptor);
}

/* ── USB-serial PTY bridge ───────────────────────────────────────────────────
 * openPty(): allocate a PTY master/slave pair WITHOUT forking a child (unlike
 * createSubprocess). NeoTerm keeps the master and pumps it to a usb-serial-for-
 * android port; proot binds the slave path onto /dev/ttyUSB*. Returns the slave
 * path, and writes the master fd into outMasterFd[0]. Returns null on failure. */
extern "C" JNIEXPORT jstring JNICALL
Java_io_neoterm_backend_JNI_openPty(JNIEnv *env, jclass TERMUX_UNUSED(clazz),
                                    jintArray outMasterFd) {
    int ptm = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
    if (ptm < 0) return nullptr;
    char devname[64];
    if (grantpt(ptm) || unlockpt(ptm) ||
#ifdef LACKS_PTSNAME_R
        strncpy(devname, ptsname(ptm) ? ptsname(ptm) : "", sizeof(devname)) == nullptr
#else
        ptsname_r(ptm, devname, sizeof(devname))
#endif
            ) {
        close(ptm);
        return nullptr;
    }
    /* Raw, transparent line discipline so serial bytes pass through unmodified. */
    struct termios tios;
    tcgetattr(ptm, &tios);
    cfmakeraw(&tios);
    tcsetattr(ptm, TCSANOW, &tios);

    jint fd = ptm;
    env->SetIntArrayRegion(outMasterFd, 0, 1, &fd);
    return env->NewStringUTF(devname);
}

static int baud_to_int(speed_t s) {
    switch (s) {
        case B50: return 50;          case B75: return 75;
        case B110: return 110;        case B134: return 134;
        case B150: return 150;        case B200: return 200;
        case B300: return 300;        case B600: return 600;
        case B1200: return 1200;      case B1800: return 1800;
        case B2400: return 2400;      case B4800: return 4800;
        case B9600: return 9600;      case B19200: return 19200;
        case B38400: return 38400;    case B57600: return 57600;
        case B115200: return 115200;  case B230400: return 230400;
        case B460800: return 460800;  case B500000: return 500000;
        case B576000: return 576000;  case B921600: return 921600;
        case B1000000: return 1000000;case B1152000: return 1152000;
        case B1500000: return 1500000;case B2000000: return 2000000;
        case B2500000: return 2500000;case B3000000: return 3000000;
        case B3500000: return 3500000;case B4000000: return 4000000;
        default: return 9600;
    }
}

/* ptySerialParams(masterFd): read the PTY's current termios (set by the guest's
 * tcsetattr on the slave) and map it to portable serial params, so the pump can
 * program the real chip via usb-serial-for-android.
 * Returns int[5] = { baud, dataBits, stopBits, parity, flow }
 *   parity: 0=none 1=odd 2=even 3=mark 4=space ;  flow: 0=none 1=rts/cts 2=xon/xoff */
extern "C" JNIEXPORT jintArray JNICALL
Java_io_neoterm_backend_JNI_ptySerialParams(JNIEnv *env, jclass TERMUX_UNUSED(clazz),
                                            jint masterFd) {
    struct termios t;
    jint out[5] = {9600, 8, 1, 0, 0};
    if (tcgetattr(masterFd, &t) == 0) {
        out[0] = baud_to_int(cfgetospeed(&t));
        switch (t.c_cflag & CSIZE) {
            case CS5: out[1] = 5; break;  case CS6: out[1] = 6; break;
            case CS7: out[1] = 7; break;  default:  out[1] = 8; break;
        }
        out[2] = (t.c_cflag & CSTOPB) ? 2 : 1;
        if (t.c_cflag & PARENB) {
#ifdef CMSPAR
            if (t.c_cflag & CMSPAR) out[3] = (t.c_cflag & PARODD) ? 3 : 4; else
#endif
            out[3] = (t.c_cflag & PARODD) ? 1 : 2;
        } else out[3] = 0;
#ifdef CRTSCTS
        if (t.c_cflag & CRTSCTS) out[4] = 1; else
#endif
        if (t.c_iflag & (IXON | IXOFF)) out[4] = 2; else out[4] = 0;
    }
    jintArray arr = env->NewIntArray(5);
    env->SetIntArrayRegion(arr, 0, 5, out);
    return arr;
}
