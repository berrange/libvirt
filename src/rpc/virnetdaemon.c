/*
 * virnetdaemon.c
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <unistd.h>
#include <fcntl.h>

#include "virnetdaemon.h"
#include "virlog.h"
#include "viralloc.h"
#include "virerror.h"
#include "virthread.h"
#include "virutil.h"
#include "virfile.h"
#include "virnetserver.h"
#include "virhash.h"
#include "virprocess.h"
#include "virsystemd.h"

#define VIR_FROM_THIS VIR_FROM_RPC

VIR_LOG_INIT("rpc.netdaemon");

typedef enum {
    VIR_NET_DAEMON_QUIT_NONE,
    VIR_NET_DAEMON_QUIT_REQUESTED,
    VIR_NET_DAEMON_QUIT_PRESERVING,
    VIR_NET_DAEMON_QUIT_READY,
    VIR_NET_DAEMON_QUIT_WAITING,
    VIR_NET_DAEMON_QUIT_COMPLETED,
} virNetDaemonQuitPhase;

#ifndef WIN32
typedef struct _virNetDaemonSignal virNetDaemonSignal;
struct _virNetDaemonSignal {
    struct sigaction oldaction;
    int signum;
    virNetDaemonSignalFunc func;
    void *opaque;
};
#endif /* !WIN32 */

struct _virNetDaemon {
    virObjectLockable parent;

    bool privileged;

#ifndef WIN32
    size_t nsignals;
    virNetDaemonSignal **signals;
    int sigread;
    int sigwrite;
    int sigwatch;
#endif /* !WIN32 */

    GHashTable *servers;
    virJSONValue *srvObject;

    virNetDaemonShutdownCallback shutdownPreserveCb;
    virNetDaemonShutdownCallback shutdownPrepareCb;
    virNetDaemonShutdownCallback shutdownWaitCb;
    virThread *shutdownPreserveThread;
    int quitTimer;
    virNetDaemonQuitPhase quit;
    bool graceful;
    bool execRestart;
    bool running; /* the daemon has reached the running phase */

    unsigned int autoShutdownTimeout;
    int autoShutdownTimerID;
    bool autoShutdownTimerActive;
    size_t autoShutdownInhibitions;
};


static virClass *virNetDaemonClass;

static int
daemonServerClose(void *payload,
                  const char *key G_GNUC_UNUSED,
                  void *opaque G_GNUC_UNUSED);

static void
virNetDaemonDispose(void *obj)
{
    virNetDaemon *dmn = obj;
#ifndef WIN32
    size_t i;

    for (i = 0; i < dmn->nsignals; i++) {
        sigaction(dmn->signals[i]->signum, &dmn->signals[i]->oldaction, NULL);
        g_free(dmn->signals[i]);
    }
    g_free(dmn->signals);
    VIR_FORCE_CLOSE(dmn->sigread);
    VIR_FORCE_CLOSE(dmn->sigwrite);
    if (dmn->sigwatch > 0)
        virEventRemoveHandle(dmn->sigwatch);
#endif /* !WIN32 */

    g_free(dmn->shutdownPreserveThread);

    g_clear_pointer(&dmn->servers, g_hash_table_unref);

    virJSONValueFree(dmn->srvObject);
}

static int
virNetDaemonOnceInit(void)
{
    if (!VIR_CLASS_NEW(virNetDaemon, virClassForObjectLockable()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virNetDaemon);


virNetDaemon *
virNetDaemonNew(void)
{
    virNetDaemon *dmn;
#ifndef WIN32
    struct sigaction sig_action = { 0 };
#endif /* !WIN32 */

    if (virNetDaemonInitialize() < 0)
        return NULL;

    if (!(dmn = virObjectLockableNew(virNetDaemonClass)))
        return NULL;

    dmn->servers = virHashNew(virObjectUnref);

#ifndef WIN32
    dmn->sigwrite = dmn->sigread = -1;
#endif /* !WIN32 */

    dmn->privileged = geteuid() == 0;

    virProcessActivateMaxFiles();

    if (virEventRegisterDefaultImpl() < 0)
        goto error;

    dmn->autoShutdownTimerID = -1;

#ifndef WIN32
    sig_action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sig_action, NULL);
#endif /* !WIN32 */

    return dmn;

 error:
    virObjectUnref(dmn);
    return NULL;
}


int
virNetDaemonAddServer(virNetDaemon *dmn,
                      virNetServer *srv)
{
    const char *serverName = virNetServerGetName(srv);
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    if (virHashAddEntry(dmn->servers, serverName, srv) < 0)
        return -1;

    virObjectRef(srv);
    return 0;
}


virNetServer *
virNetDaemonGetServer(virNetDaemon *dmn,
                      const char *serverName)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);
    virNetServer *srv = virObjectRef(virHashLookup(dmn->servers, serverName));

    if (!srv) {
        virReportError(VIR_ERR_NO_SERVER,
                       _("No server named '%1$s'"), serverName);
    }

    return srv;
}

bool
virNetDaemonHasServer(virNetDaemon *dmn,
                      const char *serverName)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    return virHashLookup(dmn->servers, serverName) != NULL;
}


struct collectData {
    virNetServer ***servers;
    size_t nservers;
};


static int
collectServers(void *payload,
               const char *name G_GNUC_UNUSED,
               void *opaque)
{
    virNetServer *srv = virObjectRef(payload);
    struct collectData *data = opaque;

    if (!srv)
        return -1;

    VIR_APPEND_ELEMENT(*data->servers, data->nservers, srv);

    return 0;
}


/*
 * Returns number of names allocated in *servers, on error sets
 * *servers to NULL and returns -1.  List of *servers must be free()d,
 * but not the items in it (similarly to virHashGetItems).
 */
ssize_t
virNetDaemonGetServers(virNetDaemon *dmn,
                       virNetServer ***servers)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);
    struct collectData data = { servers, 0 };

    *servers = NULL;

    if (virHashForEach(dmn->servers, collectServers, &data) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot get all servers from daemon"));
        virObjectListFreeCount(*servers, data.nservers);
        return -1;
    }

    return data.nservers;
}


struct virNetDaemonServerData {
    virNetDaemon *dmn;
    virNetDaemonNewServerPostExecRestart cb;
    void *opaque;
};

static int
virNetDaemonServerIterator(const char *key,
                           virJSONValue *value,
                           void *opaque)
{
    struct virNetDaemonServerData *data = opaque;
    virNetServer *srv;

    VIR_DEBUG("Creating server '%s'", key);
    srv = data->cb(data->dmn, key, value, data->opaque);
    if (!srv)
        return -1;

    if (virHashAddEntry(data->dmn->servers, key, srv) < 0)
        return -1;

    return 0;
}


virNetDaemon *
virNetDaemonNewPostExecRestart(virJSONValue *object,
                               size_t nDefServerNames,
                               const char **defServerNames,
                               virNetDaemonNewServerPostExecRestart cb,
                               void *opaque)
{
    virNetDaemon *dmn = NULL;
    virJSONValue *servers = virJSONValueObjectGet(object, "servers");
    bool new_version = virJSONValueObjectHasKey(object, "servers");

    if (!(dmn = virNetDaemonNew()))
        goto error;

    if (new_version && !servers) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Malformed servers data in JSON document"));
        goto error;
    }

    if (!new_version) {
        virNetServer *srv;

        if (nDefServerNames < 1) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No default server names provided"));
            goto error;
        }

        VIR_DEBUG("No 'servers' data, creating default '%s' name", defServerNames[0]);

        srv = cb(dmn, defServerNames[0], object, opaque);

        if (virHashAddEntry(dmn->servers, defServerNames[0], srv) < 0)
            goto error;
    } else if (virJSONValueIsArray(servers)) {
        size_t i;
        size_t n = virJSONValueArraySize(servers);
        if (n > nDefServerNames) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Server count %1$zd greater than default name count %2$zu"),
                           n, nDefServerNames);
            goto error;
        }

        for (i = 0; i < n; i++) {
            virNetServer *srv;
            virJSONValue *value = virJSONValueArrayGet(servers, i);

            VIR_DEBUG("Creating server '%s'", defServerNames[i]);
            srv = cb(dmn, defServerNames[i], value, opaque);
            if (!srv)
                goto error;

            if (virHashAddEntry(dmn->servers, defServerNames[i], srv) < 0) {
                virObjectUnref(srv);
                goto error;
            }
        }
    } else {
        struct virNetDaemonServerData data = {
            dmn,
            cb,
            opaque,
        };
        if (virJSONValueObjectForeachKeyValue(servers,
                                              virNetDaemonServerIterator,
                                              &data) < 0)
            goto error;
    }

    return dmn;

 error:
    virObjectUnref(dmn);
    return NULL;
}


virJSONValue *
virNetDaemonPreExecRestart(virNetDaemon *dmn)
{
    size_t i = 0;
    g_autoptr(virJSONValue) object = virJSONValueNewObject();
    g_autoptr(virJSONValue) srvObj = virJSONValueNewObject();
    g_autofree virHashKeyValuePair *srvArray = NULL;
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    if (!(srvArray = virHashGetItems(dmn->servers, NULL, true)))
        return NULL;

    for (i = 0; srvArray[i].key; i++) {
        virNetServer *server = virHashLookup(dmn->servers, srvArray[i].key);
        g_autoptr(virJSONValue) srvJSON = NULL;

        if (!server)
            return NULL;

        srvJSON = virNetServerPreExecRestart(server);
        if (!srvJSON)
            return NULL;

        if (virJSONValueObjectAppend(srvObj, srvArray[i].key, &srvJSON) < 0)
            return NULL;
    }

    if (virJSONValueObjectAppend(object, "servers", &srvObj) < 0)
        return NULL;

    return g_steal_pointer(&object);
}


bool
virNetDaemonIsPrivileged(virNetDaemon *dmn)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    return dmn->privileged;
}


static void
virNetDaemonAutoShutdownTimer(int timerid G_GNUC_UNUSED,
                              void *opaque)
{
    virNetDaemon *dmn = opaque;
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    if (!dmn->autoShutdownInhibitions) {
        VIR_DEBUG("Automatic shutdown triggered");
        if (dmn->quit == VIR_NET_DAEMON_QUIT_NONE) {
            VIR_DEBUG("Requesting daemon shutdown");
            dmn->quit = VIR_NET_DAEMON_QUIT_REQUESTED;
        }
    }
}


static int
virNetDaemonShutdownTimerRegister(virNetDaemon *dmn)
{
    if (dmn->autoShutdownTimerID != -1)
        return 0;

    if ((dmn->autoShutdownTimerID = virEventAddTimeout(-1,
                                                       virNetDaemonAutoShutdownTimer,
                                                       dmn, NULL)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to register shutdown timeout"));
        return -1;
    }

    return 0;
}


static void
virNetDaemonShutdownTimerUpdate(virNetDaemon *dmn)
{
    if (dmn->autoShutdownTimerID == -1)
        return;

    /* A shutdown timeout is specified, so check
     * if any drivers have active state, if not
     * shutdown after timeout seconds
     */
    if (dmn->autoShutdownTimerActive) {
        if (virNetDaemonHasClients(dmn) ||
            dmn->autoShutdownTimeout == 0) {
            VIR_DEBUG("Deactivating shutdown timer %d", dmn->autoShutdownTimerID);
            virEventUpdateTimeout(dmn->autoShutdownTimerID, -1);
            dmn->autoShutdownTimerActive = false;
        }
    } else {
        if (!virNetDaemonHasClients(dmn) &&
            dmn->autoShutdownTimeout != 0) {
            VIR_DEBUG("Activating shutdown timer %d", dmn->autoShutdownTimerID);
            virEventUpdateTimeout(dmn->autoShutdownTimerID,
                                  dmn->autoShutdownTimeout * 1000);
            dmn->autoShutdownTimerActive = true;
        }
    }
}


int
virNetDaemonAutoShutdown(virNetDaemon *dmn,
                         unsigned int timeout)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    VIR_DEBUG("Registering shutdown timeout %u", timeout);

    if (timeout > 0) {
        if (virNetDaemonShutdownTimerRegister(dmn) < 0)
            return -1;
    }

    dmn->autoShutdownTimeout = timeout;

    if (dmn->running)
        virNetDaemonShutdownTimerUpdate(dmn);

    return 0;
}


void
virNetDaemonAddShutdownInhibition(virNetDaemon *dmn)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    dmn->autoShutdownInhibitions++;

    VIR_DEBUG("dmn=%p inhibitions=%zu", dmn, dmn->autoShutdownInhibitions);
}


void
virNetDaemonRemoveShutdownInhibition(virNetDaemon *dmn)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    dmn->autoShutdownInhibitions--;

    VIR_DEBUG("dmn=%p inhibitions=%zu", dmn, dmn->autoShutdownInhibitions);
}


#ifndef WIN32
static sig_atomic_t sigErrors;
static int sigLastErrno;
static int sigWrite = -1;

static void
virNetDaemonSignalHandler(int sig, siginfo_t * siginfo,
                          void* context G_GNUC_UNUSED)
{
    int origerrno;
    int r;
    siginfo_t tmp = { 0 };

    if (SA_SIGINFO)
        tmp = *siginfo;

    /* set the sig num in the struct */
    tmp.si_signo = sig;

    origerrno = errno;
    r = safewrite(sigWrite, &tmp, sizeof(tmp));
    if (r == -1) {
        sigErrors++;
        sigLastErrno = errno;
    }
    errno = origerrno;
}

static void
virNetDaemonSignalEvent(int watch,
                        int fd G_GNUC_UNUSED,
                        int events G_GNUC_UNUSED,
                        void *opaque)
{
    virNetDaemon *dmn = opaque;
    siginfo_t siginfo;
    size_t i;

    virObjectLock(dmn);

    if (saferead(dmn->sigread, &siginfo, sizeof(siginfo)) != sizeof(siginfo)) {
        virReportSystemError(errno, "%s",
                             _("Failed to read from signal pipe"));
        virEventRemoveHandle(watch);
        dmn->sigwatch = -1;
        goto cleanup;
    }

    for (i = 0; i < dmn->nsignals; i++) {
        if (siginfo.si_signo == dmn->signals[i]->signum) {
            virNetDaemonSignalFunc func = dmn->signals[i]->func;
            void *funcopaque = dmn->signals[i]->opaque;
            virObjectUnlock(dmn);
            func(dmn, &siginfo, funcopaque);
            return;
        }
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Unexpected signal received: %1$d"), siginfo.si_signo);

 cleanup:
    virObjectUnlock(dmn);
}

static int
virNetDaemonSignalSetup(virNetDaemon *dmn)
{
    int fds[2] = { -1, -1 };

    if (dmn->sigwrite != -1)
        return 0;

    if (virPipeNonBlock(fds) < 0)
        return -1;

    if ((dmn->sigwatch = virEventAddHandle(fds[0],
                                           VIR_EVENT_HANDLE_READABLE,
                                           virNetDaemonSignalEvent,
                                           dmn, NULL)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to add signal handle watch"));
        goto error;
    }

    dmn->sigread = fds[0];
    dmn->sigwrite = fds[1];
    sigWrite = fds[1];

    return 0;

 error:
    VIR_FORCE_CLOSE(fds[0]);
    VIR_FORCE_CLOSE(fds[1]);
    return -1;
}


int
virNetDaemonAddSignalHandler(virNetDaemon *dmn,
                             int signum,
                             virNetDaemonSignalFunc func,
                             void *opaque)
{
    g_autofree virNetDaemonSignal *sigdata = NULL;
    struct sigaction sig_action = { 0 };
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    if (virNetDaemonSignalSetup(dmn) < 0)
        return -1;

    VIR_EXPAND_N(dmn->signals, dmn->nsignals, 1);

    sigdata = g_new0(virNetDaemonSignal, 1);

    sigdata->signum = signum;
    sigdata->func = func;
    sigdata->opaque = opaque;

    sig_action.sa_sigaction = virNetDaemonSignalHandler;
    sig_action.sa_flags = SA_SIGINFO;
    sigemptyset(&sig_action.sa_mask);

    sigaction(signum, &sig_action, &sigdata->oldaction);

    dmn->signals[dmn->nsignals-1] = g_steal_pointer(&sigdata);
    return 0;
}

#else /* WIN32 */

int
virNetDaemonAddSignalHandler(virNetDaemon *dmn G_GNUC_UNUSED,
                             int signum G_GNUC_UNUSED,
                             virNetDaemonSignalFunc func G_GNUC_UNUSED,
                             void *opaque G_GNUC_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Signal handling not available on this platform"));
    return -1;
}

#endif /* WIN32 */


static int
daemonServerUpdateServices(void *payload,
                           const char *key G_GNUC_UNUSED,
                           void *opaque)
{
    bool *enable = opaque;
    virNetServer *srv = payload;

    virNetServerUpdateServices(srv, *enable);
    return 0;
}

void
virNetDaemonUpdateServices(virNetDaemon *dmn,
                           bool enabled)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    virHashForEach(dmn->servers, daemonServerUpdateServices, &enabled);
}

static int
daemonServerProcessClients(void *payload,
                           const char *key G_GNUC_UNUSED,
                           void *opaque G_GNUC_UNUSED)
{
    virNetServer *srv = payload;

    virNetServerProcessClients(srv);
    return 0;
}

static int
daemonServerShutdownWait(void *payload,
                         const char *key G_GNUC_UNUSED,
                         void *opaque G_GNUC_UNUSED)
{
    virNetServer *srv = payload;

    virNetServerShutdownWait(srv);
    return 0;
}

static void
daemonShutdownWait(void *opaque)
{
    virNetDaemon *dmn = opaque;
    bool graceful = false;

    virHashForEach(dmn->servers, daemonServerShutdownWait, NULL);
    if (!dmn->shutdownWaitCb || dmn->shutdownWaitCb() >= 0)
        graceful = true;

    VIR_WITH_OBJECT_LOCK_GUARD(dmn) {
        dmn->graceful = graceful;
        dmn->quit = VIR_NET_DAEMON_QUIT_COMPLETED;
        virEventUpdateTimeout(dmn->quitTimer, 0);
        VIR_DEBUG("Shutdown wait completed graceful=%d", graceful);
    }
}

static void
virNetDaemonQuitTimer(int timerid G_GNUC_UNUSED,
                      void *opaque)
{
    virNetDaemon *dmn = opaque;
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    dmn->quit = VIR_NET_DAEMON_QUIT_COMPLETED;
    VIR_DEBUG("Shutdown wait timed out");
}


void
virNetDaemonRun(virNetDaemon *dmn)
{
    virThread shutdownThread;

    virObjectLock(dmn);

    if (dmn->srvObject) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Not all servers restored, cannot run server"));
        goto cleanup;
    }

    dmn->quit = VIR_NET_DAEMON_QUIT_NONE;
    dmn->quitTimer = -1;
    dmn->graceful = false;
    dmn->running = true;

    /* We are accepting connections now. Notify systemd
     * so it can start dependent services. */
    virSystemdNotifyReady();

    VIR_DEBUG("dmn=%p quit=%d", dmn, dmn->quit);
    while (dmn->quit != VIR_NET_DAEMON_QUIT_COMPLETED) {
        virNetDaemonShutdownTimerUpdate(dmn);

        virObjectUnlock(dmn);
        if (virEventRunDefaultImpl() < 0) {
            virObjectLock(dmn);
            VIR_DEBUG("Loop iteration error, exiting");
            break;
        }
        virObjectLock(dmn);

        virHashForEach(dmn->servers, daemonServerProcessClients, NULL);

        /* don't shutdown services when performing an exec-restart */
        if (dmn->quit == VIR_NET_DAEMON_QUIT_REQUESTED && dmn->execRestart)
            goto cleanup;

        if (dmn->quit == VIR_NET_DAEMON_QUIT_REQUESTED) {
            VIR_DEBUG("Process quit request");
            virHashForEach(dmn->servers, daemonServerClose, NULL);

            if (dmn->shutdownPreserveThread) {
                VIR_DEBUG("Shutdown preserve thread running");
                dmn->quit = VIR_NET_DAEMON_QUIT_PRESERVING;
            } else {
                VIR_DEBUG("Ready to shutdown");
                dmn->quit = VIR_NET_DAEMON_QUIT_READY;
            }
        }

        if (dmn->quit == VIR_NET_DAEMON_QUIT_READY) {
            VIR_DEBUG("Starting shutdown, running prepare");
            if (dmn->shutdownPrepareCb && dmn->shutdownPrepareCb() < 0)
                break;

            if ((dmn->quitTimer = virEventAddTimeout(30 * 1000,
                                                     virNetDaemonQuitTimer,
                                                     dmn, NULL)) < 0) {
                VIR_WARN("Failed to register finish timer.");
                break;
            }

            if (virThreadCreateFull(&shutdownThread, true, daemonShutdownWait,
                                    "daemon-shutdown", false, dmn) < 0) {
                VIR_WARN("Failed to register join thread.");
                break;
            }

            VIR_DEBUG("Waiting for shutdown completion");
            dmn->quit = VIR_NET_DAEMON_QUIT_WAITING;
        }
    }

    VIR_DEBUG("Main loop exited");
    if (dmn->graceful) {
        virThreadJoin(&shutdownThread);
        VIR_DEBUG("Graceful shutdown complete");
    } else {
        VIR_WARN("Make forcefull daemon shutdown");
        exit(EXIT_FAILURE);
    }

 cleanup:
    virObjectUnlock(dmn);
}


void
virNetDaemonQuit(virNetDaemon *dmn)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    VIR_DEBUG("Quit requested %p", dmn);
    dmn->quit = VIR_NET_DAEMON_QUIT_REQUESTED;
}


void
virNetDaemonQuitExecRestart(virNetDaemon *dmn)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    VIR_DEBUG("Exec-restart requested %p", dmn);
    dmn->quit = VIR_NET_DAEMON_QUIT_REQUESTED;
    dmn->execRestart = true;
}


static void virNetDaemonPreserveWorker(void *opaque)
{
    virNetDaemon *dmn = opaque;

    VIR_DEBUG("Begin preserve dmn=%p", dmn);

    dmn->shutdownPreserveCb();

    VIR_DEBUG("Completed preserve dmn=%p", dmn);

    VIR_WITH_OBJECT_LOCK_GUARD(dmn) {
        if (dmn->quit == VIR_NET_DAEMON_QUIT_PRESERVING) {
            VIR_DEBUG("Marking shutdown as ready");
            dmn->quit = VIR_NET_DAEMON_QUIT_READY;
        }
        g_clear_pointer(&dmn->shutdownPreserveThread, g_free);
    }

    VIR_DEBUG("End preserve dmn=%p", dmn);
    virObjectUnref(dmn);
}


void virNetDaemonPreserve(virNetDaemon *dmn)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);
    VIR_DEBUG("Preserve state request");

    if (!dmn->shutdownPreserveCb) {
        VIR_DEBUG("No preserve callback registered");
        return;
    }
    if (dmn->shutdownPreserveThread) {
        VIR_DEBUG("Preserve state thread already running");
        return;
    }

    if (dmn->quit != VIR_NET_DAEMON_QUIT_NONE) {
        VIR_WARN("Already initiated shutdown sequence, unable to preserve state");
        return;
    }

    virObjectRef(dmn);
    dmn->shutdownPreserveThread = g_new0(virThread, 1);

    if (virThreadCreateFull(dmn->shutdownPreserveThread, false,
                            virNetDaemonPreserveWorker,
                            "daemon-stop", false, dmn) < 0) {
        virObjectUnref(dmn);
        g_clear_pointer(&dmn->shutdownPreserveThread, g_free);
        return;
    }
}


static int
daemonServerClose(void *payload,
                  const char *key G_GNUC_UNUSED,
                  void *opaque G_GNUC_UNUSED)
{
    virNetServer *srv = payload;

    virNetServerClose(srv);
    return 0;
}

static int
daemonServerHasClients(void *payload,
                       const char *key G_GNUC_UNUSED,
                       void *opaque)
{
    bool *clients = opaque;
    virNetServer *srv = payload;

    if (virNetServerHasClients(srv))
        *clients = true;

    return 0;
}

bool
virNetDaemonHasClients(virNetDaemon *dmn)
{
    bool ret = false;

    virHashForEach(dmn->servers, daemonServerHasClients, &ret);

    return ret;
}

void
virNetDaemonSetShutdownCallbacks(virNetDaemon *dmn,
                                 virNetDaemonShutdownCallback preserveCb,
                                 virNetDaemonShutdownCallback prepareCb,
                                 virNetDaemonShutdownCallback waitCb)
{
    VIR_LOCK_GUARD lock = virObjectLockGuard(dmn);

    VIR_DEBUG("Shutdown callbacks preserve=%p prepare=%p wait=%p",
              preserveCb, prepareCb, waitCb);
    dmn->shutdownPreserveCb = preserveCb;
    dmn->shutdownPrepareCb = prepareCb;
    dmn->shutdownWaitCb = waitCb;
}
