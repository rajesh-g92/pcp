/*
 * Copyright (C) 2013 Red Hat.
 *
 * This file is part of the "pcp" module, the python interfaces for the
 * Performance Co-Pilot toolkit.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

/**************************************************************************\
**                                                                        **
** This C extension module mainly serves the purpose of loading functions **
** and macros needed to implement PMDAs in python.  These are exported to **
** python PMDAs via the pmda.py module, using ctypes.                     **
**                                                                        **
\**************************************************************************/

#include <Python.h>
#include <pcp/pmapi.h>
#include <pcp/pmda.h>
#include <pcp/impl.h>

static pmdaInterface dispatch;
static __pmnsTree *pmns;
static PyObject *need_refresh;

static PyObject *fetch_func;
static PyObject *refresh_func;
static PyObject *instance_func;
static PyObject *store_cb_func;
static PyObject *fetch_cb_func;

static void
pmns_refresh(void)
{
    int sts, count = 0;
    PyObject *iterator, *item;

    if (pmns)
        __pmFreePMNS(pmns);

    if ((sts = __pmNewPMNS(&pmns)) < 0) {
        __pmNotifyErr(LOG_ERR, "failed to create namespace root: %s",
                      pmErrStr(sts));
        return;
    }

    if ((iterator = PyObject_GetIter(need_refresh)) == NULL) {
        __pmNotifyErr(LOG_ERR, "failed to create metric iterator");
        return;
    }
    while ((item = PyIter_Next(iterator)) != NULL) {
        const char *name;
        long pmid;

        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
            __pmNotifyErr(LOG_ERR, "method iterator not findind 2-tuples");
            continue;
        }
        pmid = PyLong_AsLong(PyTuple_GET_ITEM(item, 0));
        name = PyString_AsString(PyTuple_GET_ITEM(item, 1));
        if ((sts = __pmAddPMNSNode(pmns, pmid, name)) < 0) {
            __pmNotifyErr(LOG_ERR,
                    "failed to add metric %s(%s) to namespace: %s",
                    name, pmIDStr(pmid), pmErrStr(sts));
        } else {
            count++;
        }
        Py_DECREF(item);
    }
    Py_DECREF(iterator);

    pmdaTreeRebuildHash(pmns, count); /* for reverse (pmid->name) lookups */
    Py_DECREF(need_refresh);
    need_refresh = NULL;
}

static PyObject *
namespace_refresh(PyObject *self, PyObject *args, PyObject *keywords)
{
    char *keyword_list[] = {"metrics", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywords,
                        "O:namespace_refresh", keyword_list, &need_refresh))
        return NULL;
    if (need_refresh)
        pmns_refresh();
    Py_INCREF(Py_None);
    return Py_None;
}

int
pmns_desc(pmID pmid, pmDesc *desc, pmdaExt *ep)
{
    if (need_refresh)
        pmns_refresh();
    return pmdaDesc(pmid, desc, ep);
}

int
pmns_pmid(const char *name, pmID *pmid, pmdaExt *pmda)
{
    if (need_refresh)
        pmns_refresh();
    return pmdaTreePMID(pmns, name, pmid);
}

int
pmns_name(pmID pmid, char ***nameset, pmdaExt *pmda)
{
    if (need_refresh)
        pmns_refresh();
    return pmdaTreeName(pmns, pmid, nameset);
}

int
pmns_children(const char *name, int traverse, char ***kids, int **sts, pmdaExt *pmda)
{
    if (need_refresh)
        pmns_refresh();
    return pmdaTreeChildren(pmns, name, traverse, kids, sts);
}

static int
prefetch(void)
{
    PyObject *arglist, *result;

    arglist = Py_BuildValue("()");
    if (arglist == NULL)
        return -ENOMEM;
    result = PyEval_CallObject(fetch_func, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        return -ENOMEM;
    Py_DECREF(result);
    return 0;
}

static int
refresh_cluster(int cluster)
{
    PyObject *arglist, *result;

    arglist = Py_BuildValue("(i)", cluster);
    if (arglist == NULL)
        return -ENOMEM;
    result = PyEval_CallObject(refresh_func, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        return -ENOMEM;
    Py_DECREF(result);
    return 0;
}

static int
refresh(int numpmid, pmID *pmidlist)
{
    size_t need;
    int *clusters = NULL;
    int i, j, count = 0;
    int sts = 0;

    /*
     * Invoke a callback once for each affected PMID cluster (and not for the
     * unaffected clusters).  This allows specific subsets of metric values to
     * be refreshed, rather than just blindly fetching everything at the start
     * of a fetch request.  Accomplish this by building an array of the unique
     * cluster numbers from the given PMID list.
     */
    need = sizeof(int) * numpmid;        /* max cluster count */
    if ((clusters = malloc(need)) == NULL)
        return -ENOMEM;
    for (i = 0; i < numpmid; i++) {
        int cluster = pmid_cluster(pmidlist[i]);
        for (j = 0; j < count; j++)
            if (clusters[j] == cluster)
                break;
        if (j == count)
            clusters[count++] = cluster;
    }
    for (j = 0; j < count; j++)
        sts |= refresh_cluster(clusters[j]);
    free(clusters);
    return sts;
}

static int
fetch(int numpmid, pmID *pmidlist, pmResult **rp, pmdaExt *pmda)
{
    int sts;

    if (need_refresh)
        pmns_refresh();
    if (fetch_func && (sts = prefetch()) < 0)
        return sts;
    if (refresh_func && (sts = refresh(numpmid, pmidlist)) < 0)
        return sts;
    return pmdaFetch(numpmid, pmidlist, rp, pmda);
}

static int
preinstance(pmInDom indom)
{
    PyObject *arglist, *result;

    arglist = Py_BuildValue("(i)", pmInDom_serial(indom));
    if (arglist == NULL)
        return -ENOMEM;
    result = PyEval_CallObject(instance_func, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        return -ENOMEM;
    Py_DECREF(result);
    return 0;
}

int
instance(pmInDom indom, int a, char *b, __pmInResult **rp, pmdaExt *pmda)
{
    int sts;

    if (need_refresh)
        pmns_refresh();
    if (instance_func && (sts = preinstance(indom)) < 0)
        return sts;
    return pmdaInstance(indom, a, b, rp, pmda);
}

int
fetch_callback(pmdaMetric *metric, unsigned int inst, pmAtomValue *atom)
{
    char *s;
    int rc, sts, code;
    PyObject *arglist, *result;
    __pmID_int *pmid = (__pmID_int *)&metric->m_desc.pmid;

    arglist = Py_BuildValue("(iii)", pmid->cluster, pmid->item, inst);
    result = PyEval_CallObject(fetch_cb_func, arglist);
    Py_DECREF(arglist);
    if (result == NULL) {
        __pmNotifyErr(LOG_ERR, "fetch callback gave no result at all");
        return -EINVAL;
    }
    rc = 0;
    sts = PMDA_FETCH_STATIC;
    switch (metric->m_desc.type) {
        case PM_TYPE_32:
            rc = PyArg_ParseTuple(result, "ii:fetch_cb", &code, &atom->l);
            break;
        case PM_TYPE_U32:
            rc = PyArg_ParseTuple(result, "iI:fetch_cb", &code, &atom->ul);
            break;
        case PM_TYPE_64:
            rc = PyArg_ParseTuple(result, "iL:fetch_cb", &code, &atom->ll);
            break;
        case PM_TYPE_U64:
            rc = PyArg_ParseTuple(result, "iK:fetch_cb", &code, &atom->ull);
            break;
        case PM_TYPE_FLOAT:
            rc = PyArg_ParseTuple(result, "if:fetch_cb", &code, &atom->f);
            break;
        case PM_TYPE_DOUBLE:
            rc = PyArg_ParseTuple(result, "id:fetch_cb", &code, &atom->d);
            break;
        case PM_TYPE_STRING:
            s = NULL;
            rc = PyArg_ParseTuple(result, "is:fetch_cb", &code, &s);
            if (rc == 0)
                break;
            if (s == NULL)
                sts = PM_ERR_VALUE;
            else if ((atom->cp = strdup(s)) == NULL)
                sts = -ENOMEM;
            else
                sts = PMDA_FETCH_DYNAMIC;
            break;
        default:
            __pmNotifyErr(LOG_ERR, "unsupported metric type in fetch callback");
            sts = -ENOTSUP;
    }
    Py_DECREF(result);

    if (rc != 0)        /* tuple successfully returned */
        return sts;

    /* expected tuple format not returned, try get an error code */
    rc = PyArg_ParseTuple(result, "i:fetch_callback_error", &code);
    if (rc == 0) {
        __pmNotifyErr(LOG_ERR, "fetch callback gave bad result (tuple expected)");
        return -EINVAL;
    }
    if (code < 0)
        return code;
    return 0;
}

int 
store_callback(__pmID_int *pmid, unsigned int inst, pmAtomValue av, int type)
{       
    int rc, code;
    int item = pmid->item;
    int cluster = pmid->cluster;
    PyObject *arglist, *result;

    switch (type) {
        case PM_TYPE_32:
            arglist = Py_BuildValue("(iiii)", cluster, item, inst, av.l);
            break;
        case PM_TYPE_U32:
            arglist = Py_BuildValue("(iiiI)", cluster, item, inst, av.ul);
            break;
        case PM_TYPE_64:
            arglist = Py_BuildValue("(iiiL)", cluster, item, inst, av.ll);
            break;
        case PM_TYPE_U64:
            arglist = Py_BuildValue("(iiiK)", cluster, item, inst, av.ull);
            break;
        case PM_TYPE_FLOAT:
            arglist = Py_BuildValue("(iiif)", cluster, item, inst, av.f);
            break;
        case PM_TYPE_DOUBLE:
            arglist = Py_BuildValue("(iiid)", cluster, item, inst, av.d);
            break;
        case PM_TYPE_STRING:
            arglist = Py_BuildValue("(iiis)", cluster, item, inst, av.cp);
            break;
        default:
            __pmNotifyErr(LOG_ERR, "unsupported type in store callback");
            return -EINVAL;
    }
    result = PyEval_CallObject(store_cb_func, arglist);
    Py_DECREF(arglist);
    rc = PyArg_ParseTuple(result, "i:store_callback", &code);
    Py_DECREF(result);
    if (rc == 0) {
        __pmNotifyErr(LOG_ERR, "store callback gave bad result (int expected)");
        return -EINVAL;
    }
    return code;
}

static pmdaMetric *
lookup_metric(__pmID_int *pmid, pmdaExt *pmda)
{
    int                i;
    pmdaMetric        *mp;

    for (i = 0; i < pmda->e_nmetrics; i++) {
        mp = &pmda->e_metrics[i];
        if (pmid->item != pmid_item(mp->m_desc.pmid))
            continue;
        if (pmid->cluster != pmid_cluster(mp->m_desc.pmid))
            continue;
        return mp;
    }
    return NULL;
}

int
store(pmResult *result, pmdaExt *pmda)
{
    int         i, j;
    int         type;
    int         sts;
    pmAtomValue av;
    pmdaMetric  *mp;
    pmValueSet  *vsp;
    __pmID_int  *pmid;

    if (need_refresh)
        pmns_refresh();

    for (i = 0; i < result->numpmid; i++) {
        vsp = result->vset[i];
        pmid = (__pmID_int *)&vsp->pmid;

        /* find the type associated with this PMID */
        if ((mp = lookup_metric(pmid, pmda)) == NULL)
            return PM_ERR_PMID;
        type = mp->m_desc.type;

        for (j = 0; j < vsp->numval; j++) {
            sts = pmExtractValue(vsp->valfmt, &vsp->vlist[j],type, &av, type);
            if (sts < 0)
                return sts;
            sts = store_callback(pmid, vsp->vlist[j].inst, av, type);
            if (sts < 0)
                return sts;
        }
    }
    return 0;
}

int
text(int ident, int type, char **buffer, pmdaExt *pmda)
{
    if (need_refresh)
        pmns_refresh();

    // TODO: iterate over the PMDA helptext dictionary
    if ((type & PM_TEXT_PMID) == PM_TEXT_PMID) {
        /* const char *hash = pmIDStr((pmID)ident); ? */
        if (type & PM_TEXT_ONELINE)
            return PM_ERR_TEXT;
        else
            return PM_ERR_TEXT;
    } else {
        /* const char *hash = pmInDomStr((pmInDom)ident); ? */
        if (type & PM_TEXT_ONELINE)
            return PM_ERR_TEXT;
        else
            return PM_ERR_TEXT;
    }
    return PM_ERR_TEXT;        /* TODO: 0 on success */
}

/*
 * Allocate a new PMDA dispatch structure and fill it
 * in for the agent we have been asked to instantiate.
 */

static inline int
pmda_generating_pmns(void) { return getenv("PCP_PYTHON_PMNS") != NULL; }

static inline int
pmda_generating_domain(void) { return getenv("PCP_PYTHON_DOMAIN") != NULL; }

static void
init_dispatch(int domain, char *name, char *logfile, char *helpfile)
{
    char *p;

    __pmSetProgname(name);
    if ((p = getenv("PCP_PYTHON_DEBUG")) != NULL)
        if ((pmDebug = __pmParseDebug(p)) < 0)
            pmDebug = 0;

    if (access(helpfile, R_OK) != 0) {
        pmdaDaemon(&dispatch, PMDA_INTERFACE_5, name, domain, logfile, NULL);
        dispatch.version.four.text = text;
    } else {
        char *help = strdup(helpfile);
        pmdaDaemon(&dispatch, PMDA_INTERFACE_5, name, domain, logfile, help);
    }
    dispatch.version.four.fetch = fetch;
    dispatch.version.four.store = store;
    dispatch.version.four.instance = instance;
    dispatch.version.four.desc = pmns_desc;
    dispatch.version.four.pmid = pmns_pmid;
    dispatch.version.four.name = pmns_name;
    dispatch.version.four.children = pmns_children;

    if (!pmda_generating_pmns() && !pmda_generating_domain())
        pmdaOpenLog(&dispatch);
}

static PyObject *
pmda_dispatch(PyObject *self, PyObject *args, PyObject *keywords)
{
    int domain;
    char *name, *help, *logfile;
    char *keyword_list[] = {"domain", "name", "log", "help", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywords,
                        "isss:pmda_dispatch", keyword_list,
                        &domain, &name, &logfile, &help))
        return NULL;

    init_dispatch(domain, name, logfile, help);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
pmda_log(PyObject *self, PyObject *args, PyObject *keywords)
{
    char *message;
    char *keyword_list[] = {"message", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywords,
                        "s:pmda_log", keyword_list, &message))
        return NULL;
    __pmNotifyErr(LOG_INFO, "%s", message);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
pmda_err(PyObject *self, PyObject *args, PyObject *keywords)
{
    char *message;
    char *keyword_list[] = {"message", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywords,
                        "s:pmda_err", keyword_list, &message))
        return NULL;
    __pmNotifyErr(LOG_ERR, "%s", message);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
pmda_pmid(PyObject *self, PyObject *args, PyObject *keywords)
{
    int result;
    int cluster, item;
    char *keyword_list[] = {"item", "cluster", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywords,
                        "ii:pmda_pmid", keyword_list,
                        &item, &cluster))
        return NULL;
    result = PMDA_PMID(item, cluster);
    return Py_BuildValue("i", result);
}

static PyObject *
pmda_units(PyObject *self, PyObject *args, PyObject *keywords)
{
    int result;
    int dim_time, dim_space, dim_count;
    int scale_space, scale_time, scale_count;
    char *keyword_list[] = {"dim_time", "dim_space", "dim_count",
                        "scale_space", "scale_time", "scale_count", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywords,
                        "iiiiii:pmda_units", keyword_list,
                        &dim_time, &dim_space, &dim_count,
                        &scale_space, &scale_time, &scale_count))
        return NULL;
    {
        pmUnits units = PMDA_PMUNITS(dim_time, dim_space, dim_count,
                                        scale_space, scale_time, scale_count);
        result = *(int *)&units;
    }
    return Py_BuildValue("i", result);
}

static PyObject *
pmda_uptime(PyObject *self, PyObject *args, PyObject *keywords)
{
    static char s[32];
    size_t sz = sizeof(s);
    int now, days, hours, mins, secs;
    char *keyword_list[] = {"seconds", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywords,
                        "i:pmda_uptime", keyword_list, &now))
        return NULL;
    
    days = now / (60 * 60 * 24);
    now %= (60 * 60 * 24);
    hours = now / (60 * 60);
    now %= (60 * 60);
    mins = now / 60;
    now %= 60;
    secs = now;

    if (days > 1)
        snprintf(s, sz, "%ddays %02d:%02d:%02d", days, hours, mins, secs);
    else if (days == 1)
        snprintf(s, sz, "%dday %02d:%02d:%02d", days, hours, mins, secs);
    else
        snprintf(s, sz, "%02d:%02d:%02d", hours, mins, secs);

    return Py_BuildValue("s", s);
}

static PyObject *
get_need_refresh(PyObject *self, PyObject *args)
{
    return Py_BuildValue("i", (need_refresh == NULL));
}

static PyObject *
set_need_refresh(PyObject *self, PyObject *args, PyObject *keywords)
{
    char *keyword_list[] = {"metrics", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywords,
                        "O:set_need_refresh", keyword_list, &need_refresh))
        return NULL;
    Py_INCREF(need_refresh);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
set_callback(PyObject *self, PyObject *args, const char *params, PyObject **callback)
{
    PyObject *func;

    if (!PyArg_ParseTuple(args, params, &func))
        return NULL;
    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "parameter must be callable");
        return NULL;
    }
    Py_XINCREF(func);
    Py_XDECREF(*callback);
    *callback = func;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
set_fetch(PyObject *self, PyObject *args)
{
    return set_callback(self, args, "O:set_fetch", &fetch_func);
}

static PyObject *
set_refresh(PyObject *self, PyObject *args)
{
    return set_callback(self, args, "O:set_refresh", &refresh_func);
}

static PyObject *
set_instance(PyObject *self, PyObject *args)
{
    return set_callback(self, args, "O:set_instance", &instance_func);
}

static PyObject *
set_store_callback(PyObject *self, PyObject *args)
{
    return set_callback(self, args, "O:set_store_callback", &store_cb_func);
}

static PyObject *
set_fetch_callback(PyObject *self, PyObject *args)
{
    return set_callback(self, args, "O:set_fetch_callback", &fetch_cb_func);
}


static PyMethodDef methods[] = {
    { .ml_name = "pmda_pmid", .ml_meth = (PyCFunction)pmda_pmid,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "pmda_units", .ml_meth = (PyCFunction)pmda_units,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "pmda_uptime", .ml_meth = (PyCFunction)pmda_uptime,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "pmda_dispatch", .ml_meth = (PyCFunction)pmda_dispatch,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "pmns_refresh", .ml_meth = (PyCFunction)namespace_refresh,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "need_refresh", .ml_meth = (PyCFunction)get_need_refresh,
        .ml_flags = METH_NOARGS },
    { .ml_name = "set_need_refresh", .ml_meth = (PyCFunction)set_need_refresh,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "set_fetch", .ml_meth = (PyCFunction)set_fetch,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "set_refresh", .ml_meth = (PyCFunction)set_refresh,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "set_instance", .ml_meth = (PyCFunction)set_instance,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "set_store_callback", .ml_meth = (PyCFunction)set_store_callback,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "set_fetch_callback", .ml_meth = (PyCFunction)set_fetch_callback,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "pmda_log", .ml_meth = (PyCFunction)pmda_log,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { .ml_name = "pmda_err", .ml_meth = (PyCFunction)pmda_err,
        .ml_flags = METH_VARARGS|METH_KEYWORDS },
    { NULL },
};

static void
pmda_dict_add(PyObject *dict, char *sym, long val)
{
    PyObject *pyVal = PyInt_FromLong(val);

    PyDict_SetItemString(dict, sym, pyVal);
    Py_XDECREF(pyVal);
}

/* called when the module is initialized. */ 
void
initcpmda(void)
{
    PyObject *module, *dict;

    module = Py_InitModule("cpmda", methods);
    dict = PyModule_GetDict(module);

    /* pmda.h - fetch callback return codes */
    pmda_dict_add(dict, "PMDA_FETCH_NOVALUES", PMDA_FETCH_NOVALUES);
    pmda_dict_add(dict, "PMDA_FETCH_STATIC", PMDA_FETCH_STATIC);
    pmda_dict_add(dict, "PMDA_FETCH_DYNAMIC", PMDA_FETCH_DYNAMIC);

    /* pmda.h - indom cache operation codes */
    pmda_dict_add(dict, "PMDA_CACHE_LOAD", PMDA_CACHE_LOAD);
    pmda_dict_add(dict, "PMDA_CACHE_ADD", PMDA_CACHE_ADD);
    pmda_dict_add(dict, "PMDA_CACHE_HIDE", PMDA_CACHE_HIDE);
    pmda_dict_add(dict, "PMDA_CACHE_CULL", PMDA_CACHE_CULL);
    pmda_dict_add(dict, "PMDA_CACHE_EMPTY", PMDA_CACHE_EMPTY);
    pmda_dict_add(dict, "PMDA_CACHE_SAVE", PMDA_CACHE_SAVE);
    pmda_dict_add(dict, "PMDA_CACHE_ACTIVE", PMDA_CACHE_ACTIVE);
    pmda_dict_add(dict, "PMDA_CACHE_INACTIVE", PMDA_CACHE_INACTIVE);
    pmda_dict_add(dict, "PMDA_CACHE_SIZE", PMDA_CACHE_SIZE);
    pmda_dict_add(dict, "PMDA_CACHE_SIZE_ACTIVE", PMDA_CACHE_SIZE_ACTIVE);
    pmda_dict_add(dict, "PMDA_CACHE_SIZE_INACTIVE", PMDA_CACHE_SIZE_INACTIVE);
    pmda_dict_add(dict, "PMDA_CACHE_REUSE", PMDA_CACHE_REUSE);
    pmda_dict_add(dict, "PMDA_CACHE_WALK_REWIND", PMDA_CACHE_WALK_REWIND);
    pmda_dict_add(dict, "PMDA_CACHE_WALK_NEXT", PMDA_CACHE_WALK_NEXT);
    pmda_dict_add(dict, "PMDA_CACHE_CHECK", PMDA_CACHE_CHECK);
    pmda_dict_add(dict, "PMDA_CACHE_REORG", PMDA_CACHE_REORG);
    pmda_dict_add(dict, "PMDA_CACHE_SYNC", PMDA_CACHE_SYNC);
    pmda_dict_add(dict, "PMDA_CACHE_DUMP", PMDA_CACHE_DUMP);
    pmda_dict_add(dict, "PMDA_CACHE_DUMP_ALL", PMDA_CACHE_DUMP_ALL);
}