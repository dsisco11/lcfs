#include "includes.h"

/* Given a layer name, find its root inode */
ino_t
lc_getRootIno(struct fs *fs, const char *name, struct inode *pdir, bool err) {
    ino_t parent = fs->fs_gfs->gfs_layerRoot;
    struct inode *dir = pdir ? pdir : fs->fs_gfs->gfs_layerRootInode;
    ino_t root;

    /* Lookup parent directory in global root file system */
    if (pdir == NULL) {
        lc_inodeLock(dir, false);
    }
    root = lc_dirLookup(fs, dir, name);
    if (pdir == NULL) {
        lc_inodeUnlock(dir);
    }
    if (unlikely(root == LC_INVALID_INODE)) {
        if (err) {
            lc_reportError(__func__, __LINE__, parent, ENOENT);
        }
    } else {
        root = lc_setHandle(lc_getIndex(fs, parent, root), root);
    }
    return root;
}

/* Link shared structures from parent */
void
lc_linkParent(struct fs *fs, struct fs *pfs) {
    fs->fs_parent = pfs;
    fs->fs_bcache = pfs->fs_bcache;
    fs->fs_rfs = pfs->fs_rfs;
    if (pfs->fs_hlinks) {
        fs->fs_hlinks = pfs->fs_hlinks;
        fs->fs_sharedHlinks = true;
    }
}

/* Invalidate pages of the first layer in kernel page cache */
static void
lc_invalidateFirstLayer(struct gfs *gfs, struct fs *pfs, int gindex) {
    struct fs *fs;

    rcu_register_thread();
    rcu_read_lock();
    fs = rcu_dereference(gfs->gfs_fs[gindex]);
    if (fs && !lc_tryLock(fs, false)) {
        rcu_read_unlock();
        lc_invalidateLayerPages(gfs, fs);
        lc_unlock(fs);
    } else {
        rcu_read_unlock();
    }
    rcu_unregister_thread();
}

/* Create a new layer */
void
lc_createLayer(fuse_req_t req, struct gfs *gfs, const char *name,
               const char *parent, size_t size, bool rw) {
    struct fs *fs = NULL, *pfs = NULL, *rfs = NULL;
    ino_t root, pinum = 0;
    struct timeval start;
    char pname[size + 1];
    int err = 0, inval;
    struct inode *pdir;
    bool base, init;
    uint32_t flags;
    size_t icsize;
    void *super;

    lc_statsBegin(&start);

    /* layers created with suffix "-init" are considered thin */
    init = rw && (strstr(name, "-init") != NULL);
    flags = LC_SUPER_DIRTY | (rw ? LC_SUPER_RDWR : 0) |
            (init ? LC_SUPER_INIT : 0);

    /* Check if parent is specified */
    if (size) {
        memcpy(pname, parent, size);
        pname[size] = 0;
        base = false;
        icsize = init ? LC_ICACHE_SIZE_MIN : LC_ICACHE_SIZE;
    } else {
        base = true;
        assert(!init);
        icsize = LC_ICACHE_SIZE_MAX;
    }

    /* Get the global file system */
    rfs = lc_getLayerLocked(LC_ROOT_INODE, false);

    /* Do not allow new layers when low on space */
    if (!lc_hasSpace(gfs, false, true)) {
        err = ENOSPC;
        lc_reportError(__func__, __LINE__, gfs->gfs_layerRoot, err);
        goto out;
    }

    /* Allocate a root inode */
    root = lc_inodeAlloc(rfs);
    pdir = gfs->gfs_layerRootInode;

    /* Find parent root inode */
    lc_inodeLock(pdir, true);
    if (!base) {
        pinum = lc_getRootIno(rfs, pname, pdir, true);
        if (pinum == LC_INVALID_INODE) {
            lc_inodeUnlock(pdir);
            err = ENOENT;
            goto out;
        }
    }

    /* Add the root inode to directory */
    lc_dirAdd(pdir, root, S_IFDIR, name, strlen(name));
    pdir->i_nlink++;
    lc_markInodeDirty(pdir, LC_INODE_DIRDIRTY);
    //lc_updateInodeTimes(pdir, true, true);
    lc_inodeUnlock(pdir);

    /* Initialize the new layer */
    fs = lc_newLayer(gfs, rw);
    lc_lock(fs, true);

    /* Initialize super block for the layer */
    lc_mallocBlockAligned(fs, (void **)&super, LC_MEMTYPE_BLOCK);
    lc_superInit(super, root, 0, flags, false);
    fs->fs_super = super;
    fs->fs_root = root;
    if (base) {
        fs->fs_rfs = fs;
    } else {
        pfs = lc_getLayerLocked(pinum, false);
        assert(pfs->fs_frozen);
        assert(rw || pfs->fs_readOnly);
        assert(pfs->fs_pcount == 0);
        assert(!(fs->fs_super->sb_flags & LC_SUPER_ZOMBIE));
        assert(pfs->fs_root == lc_getInodeHandle(pinum));
        lc_linkParent(fs, pfs);
    }

    /* Add this file system to global list of file systems */
    err = lc_addLayer(gfs, fs, pfs, &inval);

    /* If new layer could not be added, undo everything done so far */
    if (unlikely(err)) {
        lc_inodeLock(pdir, true);
        lc_dirRemove(pdir, name);
        pdir->i_nlink--;
        lc_inodeUnlock(pdir);
        goto out;
    }
    if (!rw || init) {
        __sync_add_and_fetch(&gfs->gfs_layerInProgress, 1);
    }
    lc_layerChanged(gfs, true, false);

    /* Respond now and complete the work. Operations in the layer will wait for
     * the lock on the layer.
     */
    fuse_reply_ioctl(req, 0, NULL, 0);

    /* Allocate inode cache */
    lc_icache_init(fs, icsize);

    /* Initialize the root inode */
    lc_rootInit(fs, fs->fs_root);

    if (base) {

        /* Allocate block cache for a base layer */
        lc_bcacheInit(fs, LC_PCACHE_SIZE, LC_PCLOCK_COUNT);
    } else {

        /* Copy the parent root directory */
        lc_cloneRootDir(pfs->fs_rootInode, fs->fs_rootInode);
    }

    /* Allocate stat structure if enabled */
    lc_statsNew(fs);
    lc_printf("Created fs with parent %ld root %ld index %d name %s\n",
              pfs ? pfs->fs_root : -1, root, fs->fs_gindex, name);

out:
    if (unlikely(err)) {
        fuse_reply_err(req, err);
    }
    lc_statsAdd(rfs, LC_LAYER_CREATE, err, &start);
    if (fs) {
        if (unlikely(err)) {
            fs->fs_removed = true;
            lc_unlock(fs);

            /* Shared locks on the parent layer and root layer are held to keep
             * things stable.
             */
            lc_destroyLayer(fs, true);
        } else {
            lc_unlockExclusive(fs);
        }
    }
    if (pfs) {
        if (!err && inval) {
            lc_invalidateFirstLayer(gfs, pfs, inval);
        }
        lc_unlock(pfs);
    }
    lc_unlock(rfs);
}

/* Check if a layer could be removed */
int
lc_removeRoot(struct fs *rfs, struct inode *dir, ino_t ino, bool rmdir,
               void **fsp) {
    ino_t root;

    /* There should be a file system rooted on this directory */
    root = lc_setHandle(lc_getIndex(rfs, dir->i_ino, ino), ino);
    return lc_getLayerForRemoval(rfs->fs_gfs, root, (struct fs **)fsp);
}

/* Release resources assocaited with a layer being deleted */
static void
lc_releaseLayer(struct gfs *gfs, struct fs *fs, struct fs *rfs,
                struct extent **extents) {
    struct super *super = fs->fs_super;

    assert(fs->fs_removed);
    lc_invalidateDirtyPages(gfs, fs);
    lc_invalidateInodePages(gfs, fs);
    lc_invalidateInodeBlocks(gfs, fs);
    if (super->sb_extentCount) {
        lc_addSpaceExtent(gfs, rfs, extents, super->sb_extentBlock,
                          super->sb_extentCount, true);
    }
    if (fs->fs_sblock != LC_INVALID_BLOCK) {
        lc_addSpaceExtent(gfs, rfs, extents, fs->fs_sblock, 1, true);
    }
    lc_processLayerBlocks(gfs, fs, false, true, false);
    lc_unlock(fs);
    lc_destroyLayer(fs, true);
}

/* Remove a layer */
void
lc_deleteLayer(fuse_req_t req, struct gfs *gfs, const char *name) {
    struct fs *fs = NULL, *rfs, *bfs = NULL, *zfs;
    struct extent *extents = NULL;
    struct inode *pdir = NULL;
    struct timeval start;
    int err = 0;
    ino_t root;

    /* Find the inode in layer directory */
    lc_statsBegin(&start);
    rfs = lc_getLayerLocked(LC_ROOT_INODE, false);
    pdir = gfs->gfs_layerRootInode;
    lc_inodeLock(pdir, true);

    /* Get the layer locked for removal */
    err = lc_dirRemoveName(rfs, pdir, name, true, (void **)&fs, true);
    if (unlikely(err)) {
        lc_inodeUnlock(pdir);
        fuse_reply_err(req, err);
        lc_reportError(__func__, __LINE__, pdir->i_ino, err);
        goto out;
    }

    if (fs && fs->fs_parent) {

        /* Have the base layer locked so that that will not be deleted before
         * this layer is freed.
         */
        bfs = fs->fs_rfs;
        lc_lock(bfs, false);
    }
    lc_inodeUnlock(pdir);
    fuse_reply_ioctl(req, 0, NULL, 0);
    lc_layerChanged(gfs, true, false);

    /* This could happen when a layer is made a zombie layer, which will be
     * removed when all the child layers are removed.
     */
    if (fs == NULL) {
        lc_printf("Converted layer %s to a zombie layer\n", name);
        goto out;
    }
    root = fs->fs_root;
    lc_printf("Removing fs with parent %ld root %ld name %s\n",
               fs->fs_parent ? fs->fs_parent->fs_root : - 1, root, name);

    /* Destroy pages and unlock base layer */
    zfs = fs;
    while (true) {
        lc_invalidateDirtyPages(gfs, zfs);
        lc_destroyPages(gfs, zfs, true);
        zfs = zfs->fs_zfs;
        if (zfs == NULL) {
            break;
        }
        lc_lockExclusive(zfs);
    }
    if (bfs) {
        lc_unlock(bfs);
    }

retry:
    zfs = fs->fs_zfs;
    lc_releaseLayer(gfs, fs, rfs, &extents);
    if (zfs) {

        /* Remove zombie parent layer */
        fs = zfs;
        goto retry;
    }

    /* Notify VFS about removal of a directory */
    fuse_lowlevel_notify_delete(
#ifdef FUSE3
                                gfs->gfs_se[LC_LAYER_MOUNT],
#else
                                gfs->gfs_ch[LC_LAYER_MOUNT],
#endif
                                gfs->gfs_layerRoot, root, name, strlen(name));
    if (extents) {
        lc_blockFreeExtents(gfs, rfs, extents,
                            LC_EXTENT_EFREE | LC_EXTENT_LAYER);
    }

out:
    lc_statsAdd(rfs, LC_LAYER_REMOVE, err, &start);
    lc_unlock(rfs);
}

/* Unmount a layer */
static void
lc_umountLayer(fuse_req_t req, struct gfs *gfs, ino_t root) {
    struct fs *fs = lc_getLayerLocked(root, false);
    int gindex, mcount;

    mcount = __sync_sub_and_fetch(&fs->fs_mcount, 1);
    if (mcount || fs->fs_removed) {
        lc_unlock(fs);
        fuse_reply_ioctl(req, 0, NULL, 0);
        return;
    }
    if (!fs->fs_frozen && (fs->fs_readOnly ||
                           (fs->fs_super->sb_flags & LC_SUPER_INIT))) {
        gindex = fs->fs_gindex;
        lc_unlock(fs);

        /* Allocate blocks for all dirty pages.  This must have been started by
         * release inode calls, and taking the exclusive lock make sure all
         * those operations are finished.
         */
        fs = lc_getLayerLocked(root, true);
        assert(!fs->fs_removed);
        assert((fs->fs_child == NULL) || fs->fs_commitInProgress);
        assert(!fs->fs_frozen);
        fuse_reply_ioctl(req, 0, NULL, 0);
        fs->fs_dirtyInodes = NULL;
        lc_freezeLayer(gfs, fs);

        /* Mark the layer as immutable */
        fs->fs_super->sb_lastInode = gfs->gfs_super->sb_ninode;
        fs->fs_frozen = true;
        fs->fs_commitInProgress = false;
        lc_markSuperDirty(fs);
        assert(gfs->gfs_layerInProgress > 0);
        __sync_sub_and_fetch(&gfs->gfs_layerInProgress, 1);
        lc_unlock(fs);

        /* Sync dirty data */
        rcu_register_thread();
        rcu_read_lock();
        fs = rcu_dereference(gfs->gfs_fs[gindex]);
        if (fs && (fs->fs_root == lc_getInodeHandle(root)) &&
            !lc_tryLock(fs, false)) {
            rcu_read_unlock();
            if (!fs->fs_removed) {
                lc_flushDirtyPages(gfs, fs);
                lc_processHiddenInodes(gfs, fs);
            }
            lc_unlock(fs);
        } else {
            rcu_read_unlock();
        }
        rcu_unregister_thread();
    } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
        if (fs->fs_super->sb_icount != fs->fs_icount) {
            fs->fs_super->sb_icount = fs->fs_icount;
            lc_markSuperDirty(fs);
        }
        lc_unlock(fs);
        lc_layerChanged(gfs, false, false);
    }
}

/* Mount, unmount, stat a layer */
void
lc_layerIoctl(fuse_req_t req, struct gfs *gfs, const char *name,
              enum ioctl_cmd cmd) {
    struct timeval start;
    struct fs *fs, *rfs;
    ino_t root;
    int err;

    lc_statsBegin(&start);
    rfs = lc_getLayerLocked(LC_ROOT_INODE, false);

    /* Unmount all layers */
    if (cmd == UMOUNT_ALL) {
        //ProfilerStop();
        fuse_reply_ioctl(req, 0, NULL, 0);
        lc_displayGlobalStats(gfs);
        lc_layerChanged(gfs, false, true);
        lc_statsAdd(rfs, LC_CLEANUP, 0, &start);
        lc_unlock(rfs);
        return;
    }
    if (strcmp(name, ".")) {
        root = lc_getRootIno(rfs, name, NULL, true);
        err = (root == LC_INVALID_INODE) ? ENOENT : 0;
    } else {
        root = 0;
        err = ENOENT;
    }
    switch (cmd) {
    case LAYER_MOUNT:

        /* Mark a layer as mounted */
        if (likely(err == 0)) {
            fs = lc_getLayerLocked(root, false);
            __sync_add_and_fetch(&fs->fs_mcount, 1);
            if (!fs->fs_frozen) {
                fs->fs_super->sb_flags |= LC_SUPER_DIRTY;
            }
            fuse_reply_ioctl(req, 0, NULL, 0);
            lc_unlock(fs);
        }
        lc_statsAdd(rfs, LC_MOUNT, err, &start);
        break;

    case LAYER_STAT:
        if (err == 0) {

            /* Display stats of a layer */
            fs = lc_getLayerLocked(root, false);
            fuse_reply_ioctl(req, 0, NULL, 0);
            lc_displayLayerStats(fs);
            lc_unlock(fs);
        } else if (!strcmp(name, ".")) {

            /* Display stats of all layers */
            lc_displayStatsAll(gfs);
            fuse_reply_ioctl(req, 0, NULL, 0);
            err = 0;
        }
        lc_statsAdd(rfs, LC_STAT, err, &start);
        break;

    case LAYER_UMOUNT:

        /* Unmount a layer */
        if (likely(err == 0)) {
            lc_umountLayer(req, gfs, root);
        }
        lc_statsAdd(rfs, LC_UMOUNT, err, &start);
        break;

    case CLEAR_STAT:

        /* Clear stats after displaying it */
        /* XXX Do this without locking the layer exclusive */
        if (likely(err == 0)) {
            fuse_reply_ioctl(req, 0, NULL, 0);
            fs = lc_getLayerLocked(root, true);
            if (!fs->fs_removed) {
                lc_statsDeinit(fs);
                lc_statsNew(fs);
            }
            lc_unlock(fs);
        } else if (!strcmp(name, ".")) {
            fuse_reply_ioctl(req, 0, NULL, 0);
            lc_unlock(rfs);
            lc_lock(rfs, true);
            lc_statsDeinit(rfs);
            lc_statsNew(rfs);
            err = 0;
        }
        break;

    default:
        err = EINVAL;
    }
    if (unlikely(err)) {
        lc_reportError(__func__, __LINE__, 0, err);
        fuse_reply_err(req, err);
    }
    lc_unlock(rfs);
}

/* Promote a read-write layer to read-only layer */
void
lc_commitLayer(fuse_req_t req, struct fs *fs, ino_t ino, const char *layer,
               struct fuse_file_info *fi) {
    struct fs *rfs, *cfs, *pfs, *tfs, *bfs = NULL;
    int gindex = fs->fs_gindex, newgindex;
    struct extent *extents = NULL;
    struct gfs *gfs = fs->fs_gfs;
    struct fuse_entry_param e;
    struct inode *dir;
    uint64_t blocks;
    ino_t root;

    lc_printf("Committing %s\n", layer);
    lc_copyFakeStat(&e.attr);
    e.ino = lc_setHandle(fs->fs_gindex, e.attr.st_ino);
    lc_epInit(&e);
    e.attr_timeout = 0;
    e.entry_timeout = 0;
    rfs = lc_getLayerLocked(LC_ROOT_INODE, false);
    root = lc_getRootIno(rfs, layer + strlen(LC_COMMIT_TRIGGER_PREFIX),
                         NULL, true);
    assert(root != LC_INVALID_INODE);
    lc_unlock(fs);
    cfs = lc_getLayerLocked(root, true);
    assert(!cfs->fs_removed);
    newgindex = cfs->fs_gindex;
    pfs = lc_getLayerLocked(lc_setHandle(cfs->fs_parent->fs_gindex,
                                         cfs->fs_parent->fs_root), true);
    fs = lc_getLayerLocked(ino, true);
    assert(!fs->fs_removed);

    /* Respond after locking all layers */
    fuse_reply_create(req, &e, fi);
    assert(fs->fs_aextents == NULL);

    /* Clone inodes shared with parent layers */
    tfs = pfs;
    while (tfs != fs->fs_parent) {
        lc_cloneInodes(gfs, cfs, tfs);
        tfs = tfs->fs_parent;
    }

    /* Clone root directories */
    dir = cfs->fs_rootInode;
    if (dir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(dir);
        dir = pfs->fs_rootInode;
    } else {
        dir = pfs->fs_rootInode;
        lc_dirFree(dir);
        lc_cloneRootDir(cfs->fs_rootInode, dir);
        lc_dirCopy(dir);
    }
    assert(!(dir->i_flags & LC_INODE_SHARED));

    /* Move inodes from the new layer to the layer being committed.
     * There could be open handles on inodes.
     */
    lc_moveInodes(fs, cfs);
    lc_moveRootInode(gfs, cfs, fs);

    /* Swap information in root inodes */
    lc_swapRootInode(fs, cfs);

    /* Clone root directory of the parent layer, to the new child layer */
    dir = fs->fs_rootInode;
    lc_dirFree(dir);
    lc_cloneRootDir(pfs->fs_rootInode, dir);

    /* Switch parent inode information for files in root directory */
    root = fs->fs_root;
    lc_switchInodeParent(cfs, root);
    cfs->fs_readOnly = fs->fs_readOnly;
    fs->fs_readOnly = false;
    fs->fs_pinval = -1;
    cfs->fs_pinval = -1;
    blocks = cfs->fs_mcount;
    cfs->fs_mcount = fs->fs_mcount;
    fs->fs_mcount = blocks;

    /* Switch layer roots and indices */
    assert(fs->fs_child == NULL);
    assert(gfs->gfs_roots[newgindex] == cfs->fs_root);
    assert(gfs->gfs_roots[gindex] == root);
    pthread_mutex_lock(&gfs->gfs_lock);
    fs->fs_root = cfs->fs_root;
    cfs->fs_root = root;
    fs->fs_gindex = newgindex;
    cfs->fs_gindex = gindex;
    gfs->gfs_fs[newgindex] = fs;
    gfs->gfs_fs[gindex] = cfs;

    /* Make the newly committed layer a child of the image layer */
    lc_removeChild(cfs);
    cfs->fs_prev = NULL;
    cfs->fs_next = NULL;
    cfs->fs_parent = fs->fs_parent;
    lc_addChild(gfs, fs->fs_parent, cfs);

    /* Make parent layer a child of the committed layer */
    lc_removeChild(pfs);
    pfs->fs_prev = NULL;
    pfs->fs_next = NULL;
    assert(pfs->fs_child == NULL);
    pfs->fs_parent = cfs;
    assert(cfs->fs_child == NULL);
    cfs->fs_child = pfs;

    /* Check if old parent of parent layer is pending removal */
    tfs = pfs->fs_zfs;
    if (tfs) {
        assert(tfs->fs_super->sb_flags & LC_SUPER_ZOMBIE);
        pfs->fs_zfs = NULL;
        lc_removeLayer(gfs, tfs, tfs->fs_gindex);
    }

    /* Make new child layer a child of the parent */
    lc_removeChild(fs);
    fs->fs_prev = NULL;
    fs->fs_next = NULL;
    fs->fs_parent = pfs;
    pfs->fs_child = fs;
    pthread_mutex_unlock(&gfs->gfs_lock);

    /* Update super blocks */
    fs->fs_super->sb_root = fs->fs_root;
    cfs->fs_super->sb_root = cfs->fs_root;
    fs->fs_super->sb_index = newgindex;
    cfs->fs_super->sb_index = gindex;
    cfs->fs_super->sb_lastInode = gfs->gfs_super->sb_ninode;
    if (cfs->fs_readOnly) {
        cfs->fs_super->sb_flags &= ~LC_SUPER_RDWR;
    }
    cfs->fs_super->sb_zombie = pfs->fs_gindex;
    cfs->fs_commitInProgress = true;
    fs->fs_super->sb_flags |= LC_SUPER_RDWR;
    lc_markSuperDirty(cfs);
    lc_markSuperDirty(pfs);
    lc_markSuperDirty(fs);
    if (tfs) {
        bfs = fs->fs_rfs;
        lc_lock(bfs, false);
    }
    lc_unlock(fs);
    lc_unlock(pfs);
    lc_unlock(cfs);
    if (tfs) {
        lc_lockExclusive(tfs);
        lc_invalidateDirtyPages(gfs, tfs);
        lc_destroyPages(gfs, tfs, true);
        lc_unlock(bfs);
        lc_releaseLayer(gfs, tfs, rfs, &extents);
        if (extents) {
            lc_blockFreeExtents(gfs, rfs, extents,
                                LC_EXTENT_EFREE | LC_EXTENT_LAYER);
        }
    }
    lc_unlock(rfs);
}
