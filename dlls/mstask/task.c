/*
 * Copyright (C) 2008 Google (Roy Shea)
 * Copyright (C) 2018 Dmitry Timoshkov
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "taskschd.h"
#include "mstask.h"
#include "mstask_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mstask);

typedef struct
{
    USHORT product_version;
    USHORT file_version;
    UUID uuid;
    USHORT name_size_offset;
    USHORT trigger_offset;
    USHORT error_retry_count;
    USHORT error_retry_interval;
    USHORT idle_deadline;
    USHORT idle_wait;
    UINT priority;
    UINT maximum_runtime;
    UINT exit_code;
    HRESULT status;
    UINT flags;
    SYSTEMTIME last_runtime;
} FIXDLEN_DATA;

typedef struct
{
    ITask ITask_iface;
    IPersistFile IPersistFile_iface;
    LONG ref;
    ITaskDefinition *task;
    IExecAction *action;
    LPWSTR task_name;
    HRESULT status;
    WORD idle_minutes, deadline_minutes;
    DWORD priority, maxRunTime;
    LPWSTR accountName;
    DWORD trigger_count;
    TASK_TRIGGER *trigger;
} TaskImpl;

static inline TaskImpl *impl_from_ITask(ITask *iface)
{
    return CONTAINING_RECORD(iface, TaskImpl, ITask_iface);
}

static inline TaskImpl *impl_from_IPersistFile( IPersistFile *iface )
{
    return CONTAINING_RECORD(iface, TaskImpl, IPersistFile_iface);
}

static void TaskDestructor(TaskImpl *This)
{
    TRACE("%p\n", This);
    if (This->action)
        IExecAction_Release(This->action);
    ITaskDefinition_Release(This->task);
    HeapFree(GetProcessHeap(), 0, This->task_name);
    HeapFree(GetProcessHeap(), 0, This->accountName);
    HeapFree(GetProcessHeap(), 0, This->trigger);
    HeapFree(GetProcessHeap(), 0, This);
    InterlockedDecrement(&dll_ref);
}

static HRESULT WINAPI MSTASK_ITask_QueryInterface(
        ITask* iface,
        REFIID riid,
        void **ppvObject)
{
    TaskImpl * This = impl_from_ITask(iface);

    TRACE("IID: %s\n", debugstr_guid(riid));
    if (ppvObject == NULL)
        return E_POINTER;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
            IsEqualGUID(riid, &IID_ITask))
    {
        *ppvObject = &This->ITask_iface;
        ITask_AddRef(iface);
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IPersistFile))
    {
        *ppvObject = &This->IPersistFile_iface;
        ITask_AddRef(iface);
        return S_OK;
    }

    WARN("Unknown interface: %s\n", debugstr_guid(riid));
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI MSTASK_ITask_AddRef(
        ITask* iface)
{
    TaskImpl *This = impl_from_ITask(iface);
    ULONG ref;
    TRACE("\n");
    ref = InterlockedIncrement(&This->ref);
    return ref;
}

static ULONG WINAPI MSTASK_ITask_Release(
        ITask* iface)
{
    TaskImpl * This = impl_from_ITask(iface);
    ULONG ref;
    TRACE("\n");
    ref = InterlockedDecrement(&This->ref);
    if (ref == 0)
        TaskDestructor(This);
    return ref;
}

static HRESULT WINAPI MSTASK_ITask_CreateTrigger(ITask *iface, WORD *idx, ITaskTrigger **task_trigger)
{
    TaskImpl *This = impl_from_ITask(iface);
    TASK_TRIGGER *new_trigger;
    HRESULT hr;

    TRACE("(%p, %p, %p)\n", iface, idx, task_trigger);

    hr = TaskTriggerConstructor((void **)task_trigger);
    if (hr != S_OK) return hr;

    if (This->trigger)
        new_trigger = heap_realloc(This->trigger, sizeof(This->trigger[0]) * (This->trigger_count + 1));
    else
        new_trigger = heap_alloc(sizeof(This->trigger[0]));
    if (!new_trigger)
    {
        ITaskTrigger_Release(*task_trigger);
        return E_OUTOFMEMORY;
    }

    This->trigger = new_trigger;

    hr = ITaskTrigger_GetTrigger(*task_trigger, &This->trigger[This->trigger_count]);
    if (hr == S_OK)
        *idx = This->trigger_count++;

    return hr;
}

static HRESULT WINAPI MSTASK_ITask_DeleteTrigger(ITask *iface, WORD idx)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %u)\n", iface, idx);

    if (idx >= This->trigger_count)
        return SCHED_E_TRIGGER_NOT_FOUND;

    This->trigger_count--;
    memmove(&This->trigger[idx], &This->trigger[idx + 1], (This->trigger_count - idx) * sizeof(This->trigger[0]));
    /* this shouldn't fail in practice since we're shrinking the memory block */
    This->trigger = heap_realloc(This->trigger, sizeof(This->trigger[0]) * This->trigger_count);

    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_GetTriggerCount(ITask *iface, WORD *count)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %p)\n", iface, count);

    *count = This->trigger_count;
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_GetTrigger(ITask *iface, WORD idx, ITaskTrigger **trigger)
{
    TaskImpl *This = impl_from_ITask(iface);
    HRESULT hr;

    TRACE("(%p, %u, %p)\n", iface, idx, trigger);

    if (idx >= This->trigger_count)
        return SCHED_E_TRIGGER_NOT_FOUND;

    hr = TaskTriggerConstructor((void **)trigger);
    if (hr != S_OK) return hr;

    hr = ITaskTrigger_SetTrigger(*trigger, &This->trigger[idx]);
    if (hr != S_OK)
        ITaskTrigger_Release(*trigger);

    return hr;
}

static HRESULT WINAPI MSTASK_ITask_GetTriggerString(
        ITask* iface,
        WORD iTrigger,
        LPWSTR *ppwszTrigger)
{
    FIXME("(%p, %d, %p): stub\n", iface, iTrigger, ppwszTrigger);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetRunTimes(
        ITask* iface,
        const LPSYSTEMTIME pstBegin,
        const LPSYSTEMTIME pstEnd,
        WORD *pCount,
        LPSYSTEMTIME *rgstTaskTimes)
{
    FIXME("(%p, %p, %p, %p, %p): stub\n", iface, pstBegin, pstEnd, pCount,
            rgstTaskTimes);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetNextRunTime(
        ITask* iface,
        SYSTEMTIME *pstNextRun)
{
    FIXME("(%p, %p): stub\n", iface, pstNextRun);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_SetIdleWait(
        ITask* iface,
        WORD wIdleMinutes,
        WORD wDeadlineMinutes)
{
    FIXME("(%p, %d, %d): stub\n", iface, wIdleMinutes, wDeadlineMinutes);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetIdleWait(ITask *iface, WORD *idle_minutes, WORD *deadline_minutes)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %p, %p): stub\n", iface, idle_minutes, deadline_minutes);

    *idle_minutes = This->idle_minutes;
    *deadline_minutes = This->deadline_minutes;
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_Run(
        ITask* iface)
{
    FIXME("(%p): stub\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_Terminate(
        ITask* iface)
{
    FIXME("(%p): stub\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_EditWorkItem(
        ITask* iface,
        HWND hParent,
        DWORD dwReserved)
{
    FIXME("(%p, %p, %d): stub\n", iface, hParent, dwReserved);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetMostRecentRunTime(ITask *iface, SYSTEMTIME *st)
{
    FIXME("(%p, %p): stub\n", iface, st);

    memset(st, 0, sizeof(*st));
    return SCHED_S_TASK_HAS_NOT_RUN;
}

static HRESULT WINAPI MSTASK_ITask_GetStatus(ITask *iface, HRESULT *status)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %p)\n", iface, status);

    *status = This->status;
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_GetExitCode(ITask *iface, DWORD *exit_code)
{
    FIXME("(%p, %p): stub\n", iface, exit_code);

    *exit_code = 0;
    return SCHED_S_TASK_HAS_NOT_RUN;
}

static HRESULT WINAPI MSTASK_ITask_SetComment(ITask *iface, LPCWSTR comment)
{
    TaskImpl *This = impl_from_ITask(iface);
    IRegistrationInfo *info;
    HRESULT hr;

    TRACE("(%p, %s)\n", iface, debugstr_w(comment));

    if (!comment || !comment[0])
        comment = NULL;

    hr = ITaskDefinition_get_RegistrationInfo(This->task, &info);
    if (hr == S_OK)
    {
        hr = IRegistrationInfo_put_Description(info, (BSTR)comment);
        IRegistrationInfo_Release(info);
    }
    return hr;
}

static HRESULT WINAPI MSTASK_ITask_GetComment(ITask *iface, LPWSTR *comment)
{
    TaskImpl *This = impl_from_ITask(iface);
    IRegistrationInfo *info;
    HRESULT hr;
    BSTR description;
    DWORD len;

    TRACE("(%p, %p)\n", iface, comment);

    hr = ITaskDefinition_get_RegistrationInfo(This->task, &info);
    if (hr != S_OK) return hr;

    hr = IRegistrationInfo_get_Description(info, &description);
    if (hr == S_OK)
    {
        len = description ? lstrlenW(description) + 1 : 1;
        *comment = CoTaskMemAlloc(len * sizeof(WCHAR));
        if (*comment)
        {
            if (!description)
                *comment[0] = 0;
            else
                lstrcpyW(*comment, description);
            hr = S_OK;
        }
        else
            hr =  E_OUTOFMEMORY;

        SysFreeString(description);
    }

    IRegistrationInfo_Release(info);
    return hr;
}

static HRESULT WINAPI MSTASK_ITask_SetCreator(ITask *iface, LPCWSTR creator)
{
    TaskImpl *This = impl_from_ITask(iface);
    IRegistrationInfo *info;
    HRESULT hr;

    TRACE("(%p, %s)\n", iface, debugstr_w(creator));

    if (!creator || !creator[0])
        creator = NULL;

    hr = ITaskDefinition_get_RegistrationInfo(This->task, &info);
    if (hr == S_OK)
    {
        hr = IRegistrationInfo_put_Author(info, (BSTR)creator);
        IRegistrationInfo_Release(info);
    }
    return hr;
}

static HRESULT WINAPI MSTASK_ITask_GetCreator(ITask *iface, LPWSTR *creator)
{
    TaskImpl *This = impl_from_ITask(iface);
    IRegistrationInfo *info;
    HRESULT hr;
    BSTR author;
    DWORD len;

    TRACE("(%p, %p)\n", iface, creator);

    hr = ITaskDefinition_get_RegistrationInfo(This->task, &info);
    if (hr != S_OK) return hr;

    hr = IRegistrationInfo_get_Author(info, &author);
    if (hr == S_OK)
    {
        len = author ? lstrlenW(author) + 1 : 1;
        *creator = CoTaskMemAlloc(len * sizeof(WCHAR));
        if (*creator)
        {
            if (!author)
                *creator[0] = 0;
            else
                lstrcpyW(*creator, author);
            hr = S_OK;
        }
        else
            hr =  E_OUTOFMEMORY;

        SysFreeString(author);
    }

    IRegistrationInfo_Release(info);
    return hr;
}

static HRESULT WINAPI MSTASK_ITask_SetWorkItemData(
        ITask* iface,
        WORD cBytes,
        BYTE rgbData[])
{
    FIXME("(%p, %d, %p): stub\n", iface, cBytes, rgbData);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetWorkItemData(
        ITask* iface,
        WORD *pcBytes,
        BYTE **ppBytes)
{
    FIXME("(%p, %p, %p): stub\n", iface, pcBytes, ppBytes);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_SetErrorRetryCount(
        ITask* iface,
        WORD wRetryCount)
{
    FIXME("(%p, %d): stub\n", iface, wRetryCount);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetErrorRetryCount(ITask *iface, WORD *count)
{
    TRACE("(%p, %p)\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_SetErrorRetryInterval(
        ITask* iface,
        WORD wRetryInterval)
{
    FIXME("(%p, %d): stub\n", iface, wRetryInterval);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetErrorRetryInterval(ITask *iface, WORD *interval)
{
    TRACE("(%p, %p)\n", iface, interval);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_SetFlags(
        ITask* iface,
        DWORD dwFlags)
{
    FIXME("(%p, 0x%08x): stub\n", iface, dwFlags);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetFlags(ITask *iface, DWORD *flags)
{
    FIXME("(%p, %p): stub\n", iface, flags);
    *flags = 0;
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_SetAccountInformation(
        ITask* iface,
        LPCWSTR pwszAccountName,
        LPCWSTR pwszPassword)
{
    DWORD n;
    TaskImpl *This = impl_from_ITask(iface);
    LPWSTR tmp_account_name;

    TRACE("(%p, %s, %s): partial stub\n", iface, debugstr_w(pwszAccountName),
            debugstr_w(pwszPassword));

    if (pwszPassword)
        FIXME("Partial stub ignores passwords\n");

    n = (lstrlenW(pwszAccountName) + 1);
    tmp_account_name = HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR));
    if (!tmp_account_name)
        return E_OUTOFMEMORY;
    lstrcpyW(tmp_account_name, pwszAccountName);
    HeapFree(GetProcessHeap(), 0, This->accountName);
    This->accountName = tmp_account_name;
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_GetAccountInformation(
        ITask* iface,
        LPWSTR *ppwszAccountName)
{
    DWORD n;
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %p): partial stub\n", iface, ppwszAccountName);

    /* This implements the WinXP behavior when accountName has not yet
     * set.  Win2K behaves differently, returning SCHED_E_CANNOT_OPEN_TASK */
    if (!This->accountName)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    n = (lstrlenW(This->accountName) + 1);
    *ppwszAccountName = CoTaskMemAlloc(n * sizeof(WCHAR));
    if (!*ppwszAccountName)
        return E_OUTOFMEMORY;
    lstrcpyW(*ppwszAccountName, This->accountName);
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_SetApplicationName(ITask *iface, LPCWSTR appname)
{
    TaskImpl *This = impl_from_ITask(iface);
    DWORD len;

    TRACE("(%p, %s)\n", iface, debugstr_w(appname));

    /* Empty application name */
    if (!appname || !appname[0])
        return IExecAction_put_Path(This->action, NULL);

    /* Attempt to set pwszApplicationName to a path resolved application name */
    len = SearchPathW(NULL, appname, NULL, 0, NULL, NULL);
    if (len)
    {
        LPWSTR tmp_name;
        HRESULT hr;

        tmp_name = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
        if (!tmp_name)
            return E_OUTOFMEMORY;
        len = SearchPathW(NULL, appname, NULL, len, tmp_name, NULL);
        if (len)
            hr = IExecAction_put_Path(This->action, tmp_name);
        else
            hr = HRESULT_FROM_WIN32(GetLastError());

        HeapFree(GetProcessHeap(), 0, tmp_name);
        return hr;
    }

    /* If unable to path resolve name, simply set to appname */
    return IExecAction_put_Path(This->action, (BSTR)appname);
}

static HRESULT WINAPI MSTASK_ITask_GetApplicationName(ITask *iface, LPWSTR *appname)
{
    TaskImpl *This = impl_from_ITask(iface);
    HRESULT hr;
    BSTR path;
    DWORD len;

    TRACE("(%p, %p)\n", iface, appname);

    hr = IExecAction_get_Path(This->action, &path);
    if (hr != S_OK) return hr;

    len = path ? lstrlenW(path) + 1 : 1;
    *appname = CoTaskMemAlloc(len * sizeof(WCHAR));
    if (*appname)
    {
        if (!path)
            *appname[0] = 0;
        else
            lstrcpyW(*appname, path);
        hr = S_OK;
    }
    else
        hr =  E_OUTOFMEMORY;

    SysFreeString(path);
    return hr;
}

static HRESULT WINAPI MSTASK_ITask_SetParameters(ITask *iface, LPCWSTR params)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %s)\n", iface, debugstr_w(params));

    /* Empty parameter list */
    if (!params || !params[0])
        params = NULL;

    return IExecAction_put_Arguments(This->action, (BSTR)params);
}

static HRESULT WINAPI MSTASK_ITask_GetParameters(ITask *iface, LPWSTR *params)
{
    TaskImpl *This = impl_from_ITask(iface);
    HRESULT hr;
    BSTR args;
    DWORD len;

    TRACE("(%p, %p)\n", iface, params);

    hr = IExecAction_get_Arguments(This->action, &args);
    if (hr != S_OK) return hr;

    len = args ? lstrlenW(args) + 1 : 1;
    *params = CoTaskMemAlloc(len * sizeof(WCHAR));
    if (*params)
    {
        if (!args)
            *params[0] = 0;
        else
            lstrcpyW(*params, args);
        hr = S_OK;
    }
    else
        hr =  E_OUTOFMEMORY;

    SysFreeString(args);
    return hr;
}

static HRESULT WINAPI MSTASK_ITask_SetWorkingDirectory(ITask * iface, LPCWSTR workdir)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %s)\n", iface, debugstr_w(workdir));

    if (!workdir || !workdir[0])
        workdir = NULL;

    return IExecAction_put_WorkingDirectory(This->action, (BSTR)workdir);
}

static HRESULT WINAPI MSTASK_ITask_GetWorkingDirectory(ITask *iface, LPWSTR *workdir)
{
    TaskImpl *This = impl_from_ITask(iface);
    HRESULT hr;
    BSTR dir;
    DWORD len;

    TRACE("(%p, %p)\n", iface, workdir);

    hr = IExecAction_get_WorkingDirectory(This->action, &dir);
    if (hr != S_OK) return hr;

    len = dir ? lstrlenW(dir) + 1 : 1;
    *workdir = CoTaskMemAlloc(len * sizeof(WCHAR));
    if (*workdir)
    {
        if (!dir)
            *workdir[0] = 0;
        else
            lstrcpyW(*workdir, dir);
        hr = S_OK;
    }
    else
        hr =  E_OUTOFMEMORY;

    SysFreeString(dir);
    return hr;
}

static HRESULT WINAPI MSTASK_ITask_SetPriority(
        ITask* iface,
        DWORD dwPriority)
{
    FIXME("(%p, 0x%08x): stub\n", iface, dwPriority);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetPriority(ITask *iface, DWORD *priority)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %p)\n", iface, priority);

    *priority = This->priority;
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_SetTaskFlags(
        ITask* iface,
        DWORD dwFlags)
{
    FIXME("(%p, 0x%08x): stub\n", iface, dwFlags);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_ITask_GetTaskFlags(ITask *iface, DWORD *flags)
{
    FIXME("(%p, %p): stub\n", iface, flags);
    *flags = 0;
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_SetMaxRunTime(
        ITask* iface,
        DWORD dwMaxRunTime)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %d)\n", iface, dwMaxRunTime);

    This->maxRunTime = dwMaxRunTime;
    return S_OK;
}

static HRESULT WINAPI MSTASK_ITask_GetMaxRunTime(
        ITask* iface,
        DWORD *pdwMaxRunTime)
{
    TaskImpl *This = impl_from_ITask(iface);

    TRACE("(%p, %p)\n", iface, pdwMaxRunTime);

    *pdwMaxRunTime = This->maxRunTime;
    return S_OK;
}

static HRESULT WINAPI MSTASK_IPersistFile_QueryInterface(
        IPersistFile* iface,
        REFIID riid,
        void **ppvObject)
{
    TaskImpl *This = impl_from_IPersistFile(iface);
    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), ppvObject);
    return ITask_QueryInterface(&This->ITask_iface, riid, ppvObject);
}

static ULONG WINAPI MSTASK_IPersistFile_AddRef(
        IPersistFile* iface)
{
    TaskImpl *This = impl_from_IPersistFile(iface);
    ULONG ref;
    TRACE("\n");
    ref = InterlockedIncrement(&This->ref);
    return ref;
}

static ULONG WINAPI MSTASK_IPersistFile_Release(
        IPersistFile* iface)
{
    TaskImpl *This = impl_from_IPersistFile(iface);
    ULONG ref;
    TRACE("\n");
    ref = InterlockedDecrement(&This->ref);
    if (ref == 0)
        TaskDestructor(This);
    return ref;
}

static HRESULT WINAPI MSTASK_IPersistFile_GetClassID(IPersistFile *iface, CLSID *clsid)
{
    TRACE("(%p, %p)\n", iface, clsid);

    *clsid = CLSID_CTask;
    return S_OK;
}

static HRESULT WINAPI MSTASK_IPersistFile_IsDirty(
        IPersistFile* iface)
{
    FIXME("(%p): stub\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_IPersistFile_Load(
        IPersistFile* iface,
        LPCOLESTR pszFileName,
        DWORD dwMode)
{
    FIXME("(%p, %p, 0x%08x): stub\n", iface, pszFileName, dwMode);
    return E_NOTIMPL;
}

static BOOL write_signature(HANDLE hfile)
{
    struct
    {
        USHORT SignatureVersion;
        USHORT ClientVersion;
        BYTE md5[64];
    } signature;
    DWORD size;

    signature.SignatureVersion = 0x0001;
    signature.ClientVersion = 0x0001;
    memset(&signature.md5, 0, sizeof(signature.md5));

    return WriteFile(hfile, &signature, sizeof(signature), &size, NULL);
}

static BOOL write_reserved_data(HANDLE hfile)
{
    static const struct
    {
        USHORT size;
        BYTE data[8];
    } user = { 8, { 0xff,0x0f,0x1d,0,0,0,0,0 } };
    DWORD size;

    return WriteFile(hfile, &user, sizeof(user), &size, NULL);
}

static BOOL write_user_data(HANDLE hfile, BYTE *data, WORD data_size)
{
    DWORD size;

    if (!WriteFile(hfile, &data_size, sizeof(data_size), &size, NULL))
        return FALSE;

    if (!data_size) return TRUE;

    return WriteFile(hfile, data, data_size, &size, NULL);
}

static HRESULT write_triggers(ITask *task, HANDLE hfile)
{
    WORD count, i, idx = 0xffff;
    DWORD size;
    HRESULT hr;
    ITaskTrigger *trigger;

    hr = ITask_GetTriggerCount(task, &count);
    if (hr != S_OK) return hr;

    if (!WriteFile(hfile, &count, sizeof(count), &size, NULL))
        return HRESULT_FROM_WIN32(GetLastError());

    /* Windows saves a .job with at least 1 trigger */
    if (!count)
    {
        hr = ITask_CreateTrigger(task, &idx, &trigger);
        if (hr != S_OK) return hr;
        ITaskTrigger_Release(trigger);

        count = 1;
    }

    for (i = 0; i < count; i++)
    {
        TASK_TRIGGER task_trigger;

        hr = ITask_GetTrigger(task, i, &trigger);
        if (hr != S_OK) return hr;

        hr = ITaskTrigger_GetTrigger(trigger, &task_trigger);
        ITaskTrigger_Release(trigger);
        if (hr != S_OK) break;

        if (!WriteFile(hfile, &task_trigger, sizeof(task_trigger), &size, NULL))
            return HRESULT_FROM_WIN32(GetLastError());
    }

    if (idx != 0xffff)
        ITask_DeleteTrigger(task, idx);

    return hr;
}

static BOOL write_unicode_string(HANDLE hfile, const WCHAR *str)
{
    USHORT count;
    DWORD size;

    count = str ? (lstrlenW(str) + 1) : 0;
    if (!WriteFile(hfile, &count, sizeof(count), &size, NULL))
        return FALSE;

    if (!str) return TRUE;

    count *= sizeof(WCHAR);
    return WriteFile(hfile, str, count, &size, NULL);
}

static HRESULT WINAPI MSTASK_IPersistFile_Save(IPersistFile *iface, LPCOLESTR task_name, BOOL remember)
{
    static WCHAR authorW[] = { 'W','i','n','e',0 };
    static WCHAR commentW[] = { 'C','r','e','a','t','e','d',' ','b','y',' ','W','i','n','e',0 };
    FIXDLEN_DATA fixed;
    WORD word, user_data_size = 0;
    HANDLE hfile;
    DWORD size, ver, disposition;
    TaskImpl *This = impl_from_IPersistFile(iface);
    ITask *task = &This->ITask_iface;
    LPWSTR appname = NULL, params = NULL, workdir = NULL, creator = NULL, comment = NULL;
    BYTE *user_data = NULL;
    HRESULT hr;

    TRACE("(%p, %s, %d)\n", iface, debugstr_w(task_name), remember);

    disposition = task_name ? CREATE_NEW : OPEN_ALWAYS;

    if (!task_name)
    {
        task_name = This->task_name;
        remember = FALSE;
    }

    ITask_GetComment(task, &comment);
    if (!comment) comment = commentW;
    ITask_GetCreator(task, &creator);
    if (!creator) creator = authorW;
    ITask_GetApplicationName(task, &appname);
    ITask_GetParameters(task, &params);
    ITask_GetWorkingDirectory(task, &workdir);
    ITask_GetWorkItemData(task, &user_data_size, &user_data);

    ver = GetVersion();
    fixed.product_version = MAKEWORD(ver >> 8, ver);
    fixed.file_version = 0x0001;
    CoCreateGuid(&fixed.uuid);
    fixed.name_size_offset = sizeof(fixed) + sizeof(USHORT); /* FIXDLEN_DATA + Instance Count */
    fixed.trigger_offset = sizeof(fixed) + sizeof(USHORT); /* FIXDLEN_DATA + Instance Count */
    fixed.trigger_offset += sizeof(USHORT); /* Application Name */
    if (appname)
        fixed.trigger_offset += (lstrlenW(appname) + 1) * sizeof(WCHAR);
    fixed.trigger_offset += sizeof(USHORT); /* Parameters */
    if (params)
        fixed.trigger_offset += (lstrlenW(params) + 1) * sizeof(WCHAR);
    fixed.trigger_offset += sizeof(USHORT); /* Working Directory */
    if (workdir)
        fixed.trigger_offset += (lstrlenW(workdir) + 1) * sizeof(WCHAR);
    fixed.trigger_offset += sizeof(USHORT); /* Author */
    if (creator)
        fixed.trigger_offset += (lstrlenW(creator) + 1) * sizeof(WCHAR);
    fixed.trigger_offset += sizeof(USHORT); /* Comment */
    if (comment)
        fixed.trigger_offset += (lstrlenW(comment) + 1) * sizeof(WCHAR);
    fixed.trigger_offset += sizeof(USHORT) + user_data_size; /* User Data */
    fixed.trigger_offset += 10; /* Reserved Data */

    fixed.error_retry_count = 0;
    fixed.error_retry_interval = 0;
    fixed.idle_wait = This->idle_minutes;
    fixed.idle_deadline = This->deadline_minutes;
    fixed.priority = This->priority;
    fixed.maximum_runtime = This->maxRunTime;
    fixed.exit_code = 0;
    fixed.status = SCHED_S_TASK_HAS_NOT_RUN;
    fixed.flags = 0;
    memset(&fixed.last_runtime, 0, sizeof(fixed.last_runtime));

    hfile = CreateFileW(task_name, GENERIC_WRITE, 0, NULL, disposition, 0, 0);
    if (hfile == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    if (!WriteFile(hfile, &fixed, sizeof(fixed), &size, NULL))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }

    /* Instance Count */
    word = 0;
    if (!WriteFile(hfile, &word, sizeof(word), &size, NULL))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }
    /* Application Name */
    if (!write_unicode_string(hfile, appname))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }
    /* Parameters */
    if (!write_unicode_string(hfile, params))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }
    /* Working Directory */
    if (!write_unicode_string(hfile, workdir))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }
    /* Author */
    if (!write_unicode_string(hfile, creator))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }
    /* Comment */
    if (!write_unicode_string(hfile, comment))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }

    /* User Data */
    if (!write_user_data(hfile, user_data, user_data_size))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }

    /* Reserved Data */
    if (!write_reserved_data(hfile))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }

    /* Triggers */
    hr = write_triggers(task, hfile);
    if (hr != S_OK)
        goto failed;

    /* Signature */
    if (!write_signature(hfile))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }

    hr = S_OK;

failed:
    CoTaskMemFree(appname);
    CoTaskMemFree(params);
    CoTaskMemFree(workdir);
    if (creator != authorW)
        CoTaskMemFree(creator);
    if (comment != commentW)
        CoTaskMemFree(comment);
    CoTaskMemFree(user_data);

    CloseHandle(hfile);
    if (hr != S_OK)
        DeleteFileW(task_name);
    else if (remember)
    {
        HeapFree(GetProcessHeap(), 0, This->task_name);
        This->task_name = heap_strdupW(task_name);
    }
    return hr;
}

static HRESULT WINAPI MSTASK_IPersistFile_SaveCompleted(
        IPersistFile* iface,
        LPCOLESTR pszFileName)
{
    FIXME("(%p, %p): stub\n", iface, pszFileName);
    return E_NOTIMPL;
}

static HRESULT WINAPI MSTASK_IPersistFile_GetCurFile(
        IPersistFile* iface,
        LPOLESTR *ppszFileName)
{
    FIXME("(%p, %p): stub\n", iface, ppszFileName);
    return E_NOTIMPL;
}

static const ITaskVtbl MSTASK_ITaskVtbl =
{
    MSTASK_ITask_QueryInterface,
    MSTASK_ITask_AddRef,
    MSTASK_ITask_Release,
    MSTASK_ITask_CreateTrigger,
    MSTASK_ITask_DeleteTrigger,
    MSTASK_ITask_GetTriggerCount,
    MSTASK_ITask_GetTrigger,
    MSTASK_ITask_GetTriggerString,
    MSTASK_ITask_GetRunTimes,
    MSTASK_ITask_GetNextRunTime,
    MSTASK_ITask_SetIdleWait,
    MSTASK_ITask_GetIdleWait,
    MSTASK_ITask_Run,
    MSTASK_ITask_Terminate,
    MSTASK_ITask_EditWorkItem,
    MSTASK_ITask_GetMostRecentRunTime,
    MSTASK_ITask_GetStatus,
    MSTASK_ITask_GetExitCode,
    MSTASK_ITask_SetComment,
    MSTASK_ITask_GetComment,
    MSTASK_ITask_SetCreator,
    MSTASK_ITask_GetCreator,
    MSTASK_ITask_SetWorkItemData,
    MSTASK_ITask_GetWorkItemData,
    MSTASK_ITask_SetErrorRetryCount,
    MSTASK_ITask_GetErrorRetryCount,
    MSTASK_ITask_SetErrorRetryInterval,
    MSTASK_ITask_GetErrorRetryInterval,
    MSTASK_ITask_SetFlags,
    MSTASK_ITask_GetFlags,
    MSTASK_ITask_SetAccountInformation,
    MSTASK_ITask_GetAccountInformation,
    MSTASK_ITask_SetApplicationName,
    MSTASK_ITask_GetApplicationName,
    MSTASK_ITask_SetParameters,
    MSTASK_ITask_GetParameters,
    MSTASK_ITask_SetWorkingDirectory,
    MSTASK_ITask_GetWorkingDirectory,
    MSTASK_ITask_SetPriority,
    MSTASK_ITask_GetPriority,
    MSTASK_ITask_SetTaskFlags,
    MSTASK_ITask_GetTaskFlags,
    MSTASK_ITask_SetMaxRunTime,
    MSTASK_ITask_GetMaxRunTime
};

static const IPersistFileVtbl MSTASK_IPersistFileVtbl =
{
    MSTASK_IPersistFile_QueryInterface,
    MSTASK_IPersistFile_AddRef,
    MSTASK_IPersistFile_Release,
    MSTASK_IPersistFile_GetClassID,
    MSTASK_IPersistFile_IsDirty,
    MSTASK_IPersistFile_Load,
    MSTASK_IPersistFile_Save,
    MSTASK_IPersistFile_SaveCompleted,
    MSTASK_IPersistFile_GetCurFile
};

HRESULT TaskConstructor(ITaskService *service, const WCHAR *name, ITask **task)
{
    static const WCHAR tasksW[] = { '\\','T','a','s','k','s','\\',0 };
    static const WCHAR jobW[] = { '.','j','o','b',0 };
    TaskImpl *This;
    WCHAR task_name[MAX_PATH];
    ITaskDefinition *taskdef;
    IActionCollection *actions;
    HRESULT hr;

    TRACE("(%s, %p)\n", debugstr_w(name), task);

    if (strchrW(name, '.')) return E_INVALIDARG;

    GetWindowsDirectoryW(task_name, MAX_PATH);
    lstrcatW(task_name, tasksW);
    lstrcatW(task_name, name);
    lstrcatW(task_name, jobW);

    hr = ITaskService_NewTask(service, 0, &taskdef);
    if (hr != S_OK) return hr;

    This = HeapAlloc(GetProcessHeap(), 0, sizeof(*This));
    if (!This)
    {
        ITaskDefinition_Release(taskdef);
        return E_OUTOFMEMORY;
    }

    This->ITask_iface.lpVtbl = &MSTASK_ITaskVtbl;
    This->IPersistFile_iface.lpVtbl = &MSTASK_IPersistFileVtbl;
    This->ref = 1;
    This->task = taskdef;
    This->task_name = heap_strdupW(task_name);
    This->status = SCHED_S_TASK_NOT_SCHEDULED;
    This->idle_minutes = 10;
    This->deadline_minutes = 60;
    This->priority = NORMAL_PRIORITY_CLASS;
    This->accountName = NULL;
    This->trigger_count = 0;
    This->trigger = NULL;

    /* Default time is 3 days = 259200000 ms */
    This->maxRunTime = 259200000;

    hr = ITaskDefinition_get_Actions(This->task, &actions);
    if (hr == S_OK)
    {
        hr = IActionCollection_Create(actions, TASK_ACTION_EXEC, (IAction **)&This->action);
        IActionCollection_Release(actions);
        if (hr == S_OK)
        {
            *task = &This->ITask_iface;
            InterlockedIncrement(&dll_ref);
            return S_OK;
        }
    }

    ITaskDefinition_Release(This->task);
    ITask_Release(&This->ITask_iface);
    return hr;
}
