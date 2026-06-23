package io.neoterm.backend;

/**
 * Public wrapper around the package-private {@link JNI} PTY helpers, so other
 * modules (the USB-serial bridge) can allocate a bare PTY pair and read the
 * guest's requested serial parameters.
 */
public final class Pty {
  private Pty() {}

  /** @see JNI#openPty(int[]) */
  public static String open(int[] outMasterFd) {
    return JNI.openPty(outMasterFd);
  }

  /** @see JNI#ptySerialParams(int) */
  public static int[] serialParams(int masterFd) {
    return JNI.ptySerialParams(masterFd);
  }

  /** Close a raw fd via close(2). */
  public static void close(int fd) {
    JNI.close(fd);
  }
}
