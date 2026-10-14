// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// Execution epochs. A host that runs several programs against ONE runtime (the
// JIT test harness's shared stdlib dylib) pushes an epoch before a program's
// constructors run and pops it at teardown, so every registration those
// constructors made (class objects, kernel images, manifests, calibration
// records, the backend selection, the debug location table) is gone before the
// memory it points at is.
//
// Included LAST: it reaches every registry's statics directly.

struct caj_rt_epoch {
    int classes;
    int modules;
    int manifests;
    int kparams;
    int dynshared;
    int cpuKernels;
    int census;
    int calib;
    unsigned bundled;
    int active;
    int forced;
    const CajetaDbgLocEntry* dbgEntries;
    int32_t dbgCount;
    CajetaAtExitNode* atexitHead;
};

#define CAJ_RT_EPOCH_MAX 64
static struct caj_rt_epoch g_rt_epochs[CAJ_RT_EPOCH_MAX];
static int g_rt_epoch_depth = 0;

// Saves every registry's extent and returns the new depth, or 0 when the stack is full.
__attribute__((used, retain))
int32_t __cajeta_rt_epoch_push(void) {
    if (g_rt_epoch_depth >= CAJ_RT_EPOCH_MAX) return 0;
    struct caj_rt_epoch* e = &g_rt_epochs[g_rt_epoch_depth];
    e->classes = g_cajeta_class_count;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    e->modules = g_xpu_module_count;
    e->manifests = g_xpu_manifest_count;
    e->kparams = g_xpu_kparam_count;
    e->dynshared = g_xpu_dynshared_count;
    e->cpuKernels = g_cpu_kernel_count;
    e->bundled = g_xpu_bundled;
    e->active = g_xpu_active;
    e->forced = g_xpu_forced_api;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    pthread_mutex_lock(&g_xpu_census_lock);
    e->census = g_xpu_census_count;
    pthread_mutex_unlock(&g_xpu_census_lock);
    pthread_mutex_lock(&g_xpu_calib_lock);
    e->calib = g_xpu_calib_count;
    pthread_mutex_unlock(&g_xpu_calib_lock);
    e->dbgEntries = __cajeta_dbg_loc_entries;
    e->dbgCount = __cajeta_dbg_loc_n;
    e->atexitHead = __cajeta_atexit_head;
    return ++g_rt_epoch_depth;
}

// Drops every registration made since the epoch at `depth` was pushed, and that epoch.
__attribute__((used, retain))
void __cajeta_rt_epoch_pop_to(int32_t depth) {
    if (depth < 1 || depth > g_rt_epoch_depth) return;
    const struct caj_rt_epoch* e = &g_rt_epochs[depth - 1];
    for (int i = e->classes; i < g_cajeta_class_count; ++i) {
        free((char*) g_cajeta_classes[i].name);
        g_cajeta_classes[i].name = NULL;
        g_cajeta_classes[i].classObject = NULL;
    }
    g_cajeta_class_count = e->classes;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    for (int i = e->modules; i < g_xpu_module_count; ++i)
        memset(&g_xpu_modules[i], 0, sizeof g_xpu_modules[i]);
    g_xpu_module_count = e->modules;
    g_xpu_manifest_count = e->manifests;
    g_xpu_kparam_count = e->kparams;
    g_xpu_dynshared_count = e->dynshared;
    for (int i = e->cpuKernels; i < g_cpu_kernel_count; ++i) {
        free((char*) g_cpu_kernels[i].name);
        g_cpu_kernels[i].name = NULL;
        g_cpu_kernels[i].fn = NULL;
    }
    g_cpu_kernel_count = e->cpuKernels;
    g_xpu_bundled = e->bundled;
    g_xpu_active = e->active;
    g_xpu_forced_api = e->forced;
    g_xpu_registry_gen++;
    __atomic_add_fetch(&g_xpu_manifest_gen, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    pthread_mutex_lock(&g_xpu_census_lock);
    for (int i = e->census; i < g_xpu_census_count; ++i)
        memset(&g_xpu_census[i], 0, sizeof g_xpu_census[i]);
    g_xpu_census_count = e->census;
    memset(g_xpu_census_hint, 0, sizeof g_xpu_census_hint);
    pthread_mutex_unlock(&g_xpu_census_lock);
    pthread_mutex_lock(&g_xpu_calib_lock);
    g_xpu_calib_count = e->calib;
    pthread_mutex_unlock(&g_xpu_calib_lock);
    __cajeta_dbg_loc_entries = e->dbgEntries;
    __cajeta_dbg_loc_n = e->dbgCount;
    __cajeta_inject_override_clear();
    while (__cajeta_atexit_head && __cajeta_atexit_head != e->atexitHead) {
        CajetaAtExitNode* n = __cajeta_atexit_head;
        __cajeta_atexit_head = n->next;
        free(n);
    }
    struct cajeta_anchor_table* singletons =
        __atomic_exchange_n(&__cajeta_singleton_table, NULL, __ATOMIC_ACQ_REL);
    if (singletons) __cajeta_anchor_table_release(singletons);
    g_rt_epoch_depth = depth - 1;
}
