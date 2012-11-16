/*
    server.c -- Manage a web server with one or more virtual hosts.

    A server supports multiple endpoints and one or more (virtual) hosts.
    Server Servers may be configured manually or via an "appweb.conf" configuration  file.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/

#include    "appweb.h"

/***************************** Forward Declarations ***************************/

static void manageAppweb(MaAppweb *appweb, int flags);

/************************************ Code ************************************/
/*
    Create the top level appweb control object. This is typically a singleton.
 */
PUBLIC MaAppweb *maCreateAppweb()
{
    MaAppweb    *appweb;
    Http        *http;

    if ((appweb = mprAllocObj(MaAppweb, manageAppweb)) == NULL) {
        return 0;
    }
    MPR->appwebService = appweb;
    appweb->http = http = httpCreate(HTTP_CLIENT_SIDE | HTTP_SERVER_SIDE);
    httpSetContext(http, appweb);
    appweb->servers = mprCreateList(-1, 0);
    appweb->localPlatform = slower(sfmt("%s-%s-%s", BIT_OS, BIT_CPU, BIT_PROFILE));
    maSetPlatform(appweb->localPlatform);
    maGetUserGroup(appweb);
    maParseInit(appweb);
    /* 
       Open the builtin handlers 
     */
#if BIT_PACK_DIR
    maOpenDirHandler(http);
#endif
    maOpenFileHandler(http);
    return appweb; 
}


static void manageAppweb(MaAppweb *appweb, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(appweb->defaultServer);
        mprMark(appweb->servers);
        mprMark(appweb->directives);
        mprMark(appweb->http);
        mprMark(appweb->user);
        mprMark(appweb->group);
        mprMark(appweb->localPlatform);
        mprMark(appweb->platform);
        mprMark(appweb->platformDir);

    } else if (flags & MPR_MANAGE_FREE) {
        maStopAppweb(appweb);
    }
}


PUBLIC void maAddServer(MaAppweb *appweb, MaServer *server)
{
    mprAddItem(appweb->servers, server);
}


PUBLIC void maSetDefaultServer(MaAppweb *appweb, MaServer *server)
{
    appweb->defaultServer = server;
}


PUBLIC MaServer *maLookupServer(MaAppweb *appweb, cchar *name)
{
    MaServer    *server;
    int         next;

    for (next = 0; (server = mprGetNextItem(appweb->servers, &next)) != 0; ) {
        if (strcmp(server->name, name) == 0) {
            return server;
        }
    }
    return 0;
}


PUBLIC int maStartAppweb(MaAppweb *appweb)
{
    MaServer    *server;
    char        *timeText;
    int         next;

    for (next = 0; (server = mprGetNextItem(appweb->servers, &next)) != 0; ) {
        if (maStartServer(server) < 0) {
            return MPR_ERR_CANT_INITIALIZE;
        }
    }
    timeText = mprGetDate(0);
    mprLog(1, "HTTP services Started at %s with max %d threads", timeText, mprGetMaxWorkers(appweb));
    return 0;
}


PUBLIC int maStopAppweb(MaAppweb *appweb)
{
    MaServer  *server;
    int     next;

    for (next = 0; (server = mprGetNextItem(appweb->servers, &next)) != 0; ) {
        maStopServer(server);
    }
    return 0;
}


static void manageServer(MaServer *server, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(server->name);
        mprMark(server->appweb);
        mprMark(server->http);
        mprMark(server->limits);
        mprMark(server->endpoints);
        mprMark(server->home);

    } else if (flags & MPR_MANAGE_FREE) {
        maStopServer(server);
    }
}


/*  
    Create a new server. A server may manage may multiple servers and virtual hosts. 
    If ip/port endpoint is supplied, this call will create a Server on that endpoint. Otherwise, 
    maConfigureServer should be called later. A default route is created with the document root set to "."
 */
PUBLIC MaServer *maCreateServer(MaAppweb *appweb, cchar *name)
{
    MaServer    *server;
    HttpHost    *host;
    HttpRoute   *route;

    assure(appweb);

    if ((server = mprAllocObj(MaServer, manageServer)) == NULL) {
        return 0;
    }
    if (name == 0 || *name == '\0') {
        name = "default";
    }
    server->name = sclone(name);
    server->endpoints = mprCreateList(-1, 0);
    server->limits = httpCreateLimits(1);
    server->appweb = appweb;
    server->http = appweb->http;

    server->defaultHost = host = httpCreateHost(NULL);
    if (!httpGetDefaultHost()) {
        httpSetDefaultHost(host);
    }
    route = httpCreateRoute(host);
    httpSetHostDefaultRoute(host, route);
    route->limits = server->limits;

    maAddServer(appweb, server);
    if (appweb->defaultServer == 0) {
        maSetDefaultServer(appweb, server);
    }
    return server;
}


/*
    Configure the server. If the configFile is defined, use it. If not, then consider home, documents, ip and port.
 */
PUBLIC int maConfigureServer(MaServer *server, cchar *configFile, cchar *home, cchar *documents, cchar *ip, int port)
{
    MaAppweb        *appweb;
    Http            *http;
    HttpEndpoint    *endpoint;
    HttpHost        *host;
    HttpRoute       *route;
    char            *path;

    appweb = server->appweb;
    http = appweb->http;

    if (configFile) {
        path = mprGetAbsPath(configFile);
        if (maParseConfig(server, path, 0) < 0) {
            /* mprUserError("Cannot configure server using %s", path); */
            return MPR_ERR_CANT_INITIALIZE;
        }
        return 0;

    } else {
        mprLog(2, "DocumentRoot %s", documents);
        if ((endpoint = httpCreateConfiguredEndpoint(home, documents, ip, port)) == 0) {
            return MPR_ERR_CANT_OPEN;
        }
        maAddEndpoint(server, endpoint);
        host = mprGetFirstItem(endpoint->hosts);
        assure(host);
        route = mprGetFirstItem(host->routes);
        assure(route);

#if BIT_PACK_CGI
        maLoadModule(appweb, "cgiHandler", "mod_cgi");
        if (httpLookupStage(http, "cgiHandler")) {
            httpAddRouteHandler(route, "cgiHandler", "cgi cgi-nph bat cmd pl py");
            /*
                Add cgi-bin with a route for the /cgi-bin URL prefix.
             */
            path = "cgi-bin";
            if (mprPathExists(path, X_OK)) {
                HttpRoute *cgiRoute;
                cgiRoute = httpCreateAliasRoute(route, "/cgi-bin/", path, 0);
                mprLog(4, "ScriptAlias \"/cgi-bin/\":\"%s\"", path);
                httpSetRouteHandler(cgiRoute, "cgiHandler");
                httpFinalizeRoute(cgiRoute);
            }
        }
#endif
#if BIT_PACK_ESP
        maLoadModule(appweb, "espHandler", "mod_esp");
        if (httpLookupStage(http, "espHandler")) {
            httpAddRouteHandler(route, "espHandler", "esp");
        }
#endif
#if BIT_PACK_EJSCRIPT
        maLoadModule(appweb, "ejsHandler", "mod_ejs");
        if (httpLookupStage(http, "ejsHandler")) {
            httpAddRouteHandler(route, "ejsHandler", "ejs");
        }
#endif
#if BIT_PACK_PHP
        maLoadModule(appweb, "phpHandler", "mod_php");
        if (httpLookupStage(http, "phpHandler")) {
            httpAddRouteHandler(route, "phpHandler", "php");
        }
#endif
        httpAddRouteHandler(route, "fileHandler", "");
        httpFinalizeRoute(route);
    }
    if (home) {
        maSetServerHome(server, home);
    }
    if (ip || port > 0) {
        maSetServerAddress(server, ip, port);
    }
    return 0;
}


PUBLIC int maStartServer(MaServer *server)
{
    HttpEndpoint    *endpoint;
    int             next, count, warned;

    warned = 0;
    count = 0;
    for (next = 0; (endpoint = mprGetNextItem(server->endpoints, &next)) != 0; ) {
        if (httpStartEndpoint(endpoint) < 0) {
            warned++;
            break;
        } else {
            count++;
        }
    }
    if (count == 0) {
        if (!warned) {
            mprError("Server is not listening on any addresses");
        }
        return MPR_ERR_CANT_OPEN;
    }
#if BIT_UNIX_LIKE
    MaAppweb    *appweb = server->appweb;
    if (appweb->userChanged || appweb->groupChanged) {
        if (!smatch(MPR->logPath, "stdout") && !smatch(MPR->logPath, "stderr")) {
            if (chown(MPR->logPath, appweb->uid, appweb->gid) < 0) {
                mprError("Cannot change ownership on %s", MPR->logPath);
            }
        }
    }
    if (maApplyChangedGroup(appweb) < 0 || maApplyChangedUser(appweb) < 0) {
        return MPR_ERR_CANT_COMPLETE;
    }
#endif
    return 0;
}


PUBLIC void maStopServer(MaServer *server)
{
    HttpEndpoint    *endpoint;
    int             next;

    for (next = 0; (endpoint = mprGetNextItem(server->endpoints, &next)) != 0; ) {
        httpStopEndpoint(endpoint);
    }
}


PUBLIC void maAddEndpoint(MaServer *server, HttpEndpoint *endpoint)
{
    mprAddItem(server->endpoints, endpoint);
}


PUBLIC void maRemoveEndpoint(MaServer *server, HttpEndpoint *endpoint)
{
    mprRemoveItem(server->endpoints, endpoint);
}


PUBLIC int maSetPlatform(cchar *platform)
{
    MprDirEntry *dp;
    MaAppweb    *appweb;
    cchar       *base, *dir, *junk;
    int         next;

    appweb = MPR->appwebService;
    if (mprSamePath(mprGetAppDir(), BIT_BIN_PREFIX)) {
        /* Installed */
        base = mprGetPathParent(mprGetAppDir());
        dir = smatch(platform, appweb->localPlatform) ? base : mprJoinPath(base, platform);
    } else {
        /* Local Dev */
        base = mprGetPathParent(mprGetPathParent(mprGetAppDir()));
        dir = mprJoinPath(base, platform);
    }
    if (!mprIsPathDir(dir)) {
        for (ITERATE_ITEMS(mprGetPathFiles(base, 0), dp, next)) {
            if (dp->isDir && sstarts(mprGetPathBase(dp->name), platform)) {
                platform = mprGetPathBase(dp->name);
                dir = dp->name;
                if (maParsePlatform(platform, &junk, &junk, &junk) == 0) {
                    break;
                }
            }
        }
        if (!dp) {
            return MPR_ERR_BAD_ARGS;
        }
    }
    if (maParsePlatform(platform, &junk, &junk, &junk) < 0) {
        return MPR_ERR_BAD_ARGS;
    }
    appweb->platformDir = dir;
    appweb->platform = platform;
    return 0;
}

/*  
    Set the home directory (Server Root). We convert path into an absolute path.
 */
PUBLIC void maSetServerHome(MaServer *server, cchar *path)
{
    if (path == 0 || BIT_ROM) {
        path = ".";
    }
#if !VXWORKS
    /*
        VxWorks stat() is broken if using a network FTP server.
     */
    if (! mprPathExists(path, R_OK)) {
        mprError("Cannot access ServerRoot directory %s", path);
        return;
    }
#endif
    server->home = mprGetAbsPath(path);
    mprLog(MPR_CONFIG, "Set server root to: \"%s\"", server->home);
}


/*
    Set the document root for the default server
 */
PUBLIC void maSetServerAddress(MaServer *server, cchar *ip, int port)
{
    HttpEndpoint    *endpoint;
    int             next;

    for (next = 0; ((endpoint = mprGetNextItem(server->endpoints, &next)) != 0); ) {
        httpSetEndpointAddress(endpoint, ip, port);
    }
}


PUBLIC void maGetUserGroup(MaAppweb *appweb)
{
#if BIT_UNIX_LIKE
    struct passwd   *pp;
    struct group    *gp;

    appweb->uid = getuid();
    if ((pp = getpwuid(appweb->uid)) == 0) {
        mprError("Cannot read user credentials: %d. Check your /etc/passwd file.", appweb->uid);
    } else {
        appweb->user = sclone(pp->pw_name);
    }
    appweb->gid = getgid();
    if ((gp = getgrgid(appweb->gid)) == 0) {
        mprError("Cannot read group credentials: %d. Check your /etc/group file", appweb->gid);
    } else {
        appweb->group = sclone(gp->gr_name);
    }
#else
    appweb->uid = appweb->gid = -1;
#endif
}


PUBLIC int maSetHttpUser(MaAppweb *appweb, cchar *newUser)
{
#if BIT_UNIX_LIKE
    struct passwd   *pp;

    if (snumber(newUser)) {
        appweb->uid = atoi(newUser);
        if ((pp = getpwuid(appweb->uid)) == 0) {
            mprError("Bad user id: %d", appweb->uid);
            return MPR_ERR_CANT_ACCESS;
        }
        newUser = pp->pw_name;

    } else {
        if ((pp = getpwnam(newUser)) == 0) {
            mprError("Bad user name: %s", newUser);
            return MPR_ERR_CANT_ACCESS;
        }
        appweb->uid = pp->pw_uid;
    }
    appweb->userChanged = 1;
#endif
    appweb->user = sclone(newUser);
    return 0;
}


PUBLIC int maSetHttpGroup(MaAppweb *appweb, cchar *newGroup)
{
#if BIT_UNIX_LIKE
    struct group    *gp;

    if (snumber(newGroup)) {
        appweb->gid = atoi(newGroup);
        if ((gp = getgrgid(appweb->gid)) == 0) {
            mprError("Bad group id: %d", appweb->gid);
            return MPR_ERR_CANT_ACCESS;
        }
        newGroup = gp->gr_name;

    } else {
        if ((gp = getgrnam(newGroup)) == 0) {
            mprError("Bad group name: %s", newGroup);
            return MPR_ERR_CANT_ACCESS;
        }
        appweb->gid = gp->gr_gid;
    }
    appweb->groupChanged = 1;
#endif
    appweb->group = sclone(newGroup);
    return 0;
}


PUBLIC int maApplyChangedUser(MaAppweb *appweb)
{
#if BIT_UNIX_LIKE
    if (appweb->userChanged && appweb->uid >= 0) {
        if ((setuid(appweb->uid)) != 0) {
            mprError("Cannot change user to: %s: %d\n"
                "WARNING: This is a major security exposure", appweb->user, appweb->uid);
            return MPR_ERR_BAD_STATE;
#if LINUX && PR_SET_DUMPABLE
        } else {
            prctl(PR_SET_DUMPABLE, 1);
#endif
        }
        mprLog(MPR_CONFIG, "Changing user ID to %s: %d", appweb->user, appweb->uid);
    }
#endif
    return 0;
}


PUBLIC int maApplyChangedGroup(MaAppweb *appweb)
{
#if BIT_UNIX_LIKE
    if (appweb->groupChanged && appweb->gid >= 0) {
        if (setgid(appweb->gid) != 0) {
            mprError("Cannot change group to %s: %d\n"
                "WARNING: This is a major security exposure", appweb->group, appweb->gid);
            return MPR_ERR_BAD_STATE;
#if LINUX && PR_SET_DUMPABLE
        } else {
            prctl(PR_SET_DUMPABLE, 1);
#endif
        }
        mprLog(MPR_CONFIG, "Changing group ID to %s: %d", appweb->group, appweb->gid);
    }
#endif
    return 0;
}


/*
    Load a module. Returns 0 if the modules is successfully loaded (may have already been loaded).
 */
PUBLIC int maLoadModule(MaAppweb *appweb, cchar *name, cchar *libname)
{
    MprModule   *module;
    char        entryPoint[MPR_MAX_FNAME];
    char        *path;

    if (strcmp(name, "authFilter") == 0 || strcmp(name, "rangeFilter") == 0 || strcmp(name, "uploadFilter") == 0 ||
            strcmp(name, "fileHandler") == 0 || strcmp(name, "dirHandler") == 0) {
        mprLog(1, "The %s module is now builtin. No need to use LoadModule", name);
        return 0;
    }
    if ((module = mprLookupModule(name)) != 0) {
        mprLog(MPR_CONFIG, "Activating module (Builtin) %s", name);
        return 0;
    }
    if (libname == 0) {
        path = sjoin("mod_", name, BIT_SHOBJ, NULL);
    } else {
        path = sclone(libname);
    }
    fmt(entryPoint, sizeof(entryPoint), "ma%sInit", name);
    entryPoint[2] = toupper((uchar) entryPoint[2]);
    if ((module = mprCreateModule(name, path, entryPoint, MPR->httpService)) == 0) {
        return 0;
    }
    if (mprLoadModule(module) < 0) {
        return MPR_ERR_CANT_CREATE;
    }
    return 0;
}

 
PUBLIC HttpAuth *maGetDefaultAuth(MaServer *server)
{
    return server->defaultHost->defaultRoute->auth;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
