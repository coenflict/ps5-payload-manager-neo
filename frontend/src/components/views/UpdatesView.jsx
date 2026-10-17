import React, { useState, useEffect, useCallback, useRef } from 'react'
import {
  ArrowLeft, RefreshCw, Loader2, ShieldCheck, Download, RotateCcw,
  ChevronDown, ChevronUp, AlertTriangle, CheckCircle2, CloudOff, Package
} from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { cn } from '../../utils/helpers'
import { apiFetch, mutationFetch } from '../../utils/mutationRequest'
import { createRequestScope } from '../../utils/requestScope'

const POLICIES = ['safe', 'auto', 'notify', 'off']
const BUDGETS = [10, 20, 40, 60]
const ACTIONABLE = ['available', 'approval', 'failed']

const Toggle = ({ value, onChange, disabled }) => (
  <button
    onClick={() => onChange(!value)}
    disabled={disabled}
    className={cn('w-14 h-7 md:w-20 md:h-10 rounded-full transition-all relative p-1 md:p-1.5 shrink-0 disabled:opacity-50',
      value ? 'bg-ps-blue' : 'bg-white/10')}
  >
    <div className={cn('w-5 h-5 md:w-7 md:h-7 bg-white rounded-full transition-all',
      value ? 'translate-x-7 md:translate-x-10' : 'translate-x-0')} />
  </button>
)

const stateStyle = state => ({
  updated: 'bg-emerald-500/15 text-emerald-300 border-emerald-500/30',
  current: 'bg-white/5 text-zinc-300 border-white/10',
  available: 'bg-ps-blue/15 text-ps-blue border-ps-blue/30',
  approval: 'bg-amber-500/15 text-amber-300 border-amber-500/30',
  failed: 'bg-red-500/15 text-red-300 border-red-500/30',
  offline: 'bg-white/5 text-zinc-400 border-white/10',
}[state] || 'bg-white/5 text-zinc-400 border-white/10')

const formatTime = seconds => {
  if (!seconds) return null
  try {
    return new Date(seconds * 1000).toLocaleString()
  } catch {
    return null
  }
}

const PayloadRow = ({ item, busy, onPolicy, onApply, onRollback, onAdvanced, onUnskip }) => {
  const { t } = useTranslation()
  const [open, setOpen] = useState(false)
  const [repo, setRepo] = useState(item.repo)
  const [asset, setAsset] = useState(item.asset)
  const stateLabels = {
    idle: t('updates.state_idle', 'Not checked yet'),
    pending: t('updates.state_pending', 'Waiting'),
    checking: t('updates.state_checking', 'Checking'),
    downloading: t('updates.state_downloading', 'Installing'),
    current: t('updates.state_current', 'Up to date'),
    updated: t('updates.state_updated', 'Updated'),
    available: t('updates.state_available', 'Update available'),
    approval: t('updates.state_approval', 'Needs approval'),
    failed: t('updates.state_failed', 'Update failed'),
    offline: t('updates.state_offline', 'Offline'),
    disabled: t('updates.state_disabled', 'Updates off'),
    untracked: t('updates.state_untracked', 'No update source'),
    skipped: t('updates.state_skipped', 'Skipped'),
  }
  const policyLabels = {
    safe: t('updates.policy_safe', 'Safe: automatic, major versions need approval'),
    auto: t('updates.policy_auto', 'Automatic: install every new version'),
    notify: t('updates.policy_notify', 'Notify only'),
    off: t('updates.policy_off', 'Off'),
  }
  const canInstall = item.latest && ACTIONABLE.includes(item.state)
  return (
    <div className="p-4 md:p-6 bg-white/[0.03] rounded-3xl border border-white/10 space-y-4">
      <div className="flex flex-col md:flex-row md:items-center md:justify-between gap-3">
        <div className="min-w-0 space-y-1">
          <div className="flex flex-wrap items-center gap-2">
            <p className="font-bold text-white text-lg tracking-tight truncate">{item.name}</p>
            {item.autoload && (
              <span className="text-[11px] font-bold uppercase tracking-wide px-2 py-0.5 rounded-md bg-ps-blue/15 text-ps-blue">
                {t('updates.autoload_badge', 'Autoload')}
              </span>
            )}
            <span className={cn('text-xs font-bold px-2 py-0.5 rounded-md border', stateStyle(item.state))}>
              {stateLabels[item.state] || item.state}
            </span>
          </div>
          <p className="text-sm text-zinc-400">
            {item.installed || t('updates.unknown_version', 'unknown version')}
            {item.latest && item.latest !== item.installed && (
              <span className="text-zinc-300"> → {item.latest}
                <span className="text-zinc-500"> ({item.source === 'upstream'
                  ? t('updates.source_upstream', 'maintainer release')
                  : t('updates.source_catalog', 'catalog')})</span>
              </span>
            )}
          </p>
          {item.message && <p className="text-xs text-zinc-500 break-words">{item.message}</p>}
          {item.skip && (
            <p className="text-xs text-amber-300/80">
              {t('updates.skipping', 'Skipping {{version}}', { version: item.skip })}{' '}
              <button onClick={() => onUnskip(item)} className="underline hover:text-white">
                {t('updates.allow_again', 'Allow again')}
              </button>
            </p>
          )}
        </div>
        <div className="flex flex-wrap items-center gap-2 shrink-0">
          {canInstall && (
            <button onClick={() => onApply(item)} disabled={busy}
              className="inline-flex items-center gap-2 px-4 py-2 rounded-xl bg-ps-blue hover:bg-[#007acc] text-white font-bold text-sm disabled:opacity-50">
              {busy ? <Loader2 className="w-4 h-4 animate-spin" /> : <Download className="w-4 h-4" />}
              {t('updates.install_version', 'Install {{version}}', { version: item.latest })}
            </button>
          )}
          {item.has_previous && (
            <button onClick={() => onRollback(item)} disabled={busy}
              className="inline-flex items-center gap-2 px-4 py-2 rounded-xl bg-white/5 hover:bg-white/10 border border-white/10 text-white font-bold text-sm disabled:opacity-50">
              <RotateCcw className="w-4 h-4" />
              {t('updates.restore_version', 'Restore {{version}}', { version: item.previous || t('updates.previous', 'previous') })}
            </button>
          )}
        </div>
      </div>
      <div className="flex flex-col md:flex-row md:items-center gap-3">
        <select value={item.policy} onChange={e => onPolicy(item, { policy: e.target.value })}
          aria-label={t('updates.policy_label', 'Update policy')}
          className="bg-black/50 border border-white/10 text-white rounded-xl px-3 py-2 text-sm font-bold md:max-w-md">
          {POLICIES.map(p => <option key={p} value={p} className="bg-[#121214]">{policyLabels[p]}</option>)}
        </select>
        <button onClick={() => setOpen(!open)} className="inline-flex items-center gap-1 text-sm text-zinc-400 hover:text-white">
          {open ? <ChevronUp className="w-4 h-4" /> : <ChevronDown className="w-4 h-4" />}
          {t('updates.advanced', 'Advanced')}
          {item.upstream && <span className="text-zinc-600 ml-2 truncate">{item.upstream}{item.pattern ? ` · ${item.pattern}` : ''}</span>}
        </button>
      </div>
      {open && (
        <div className="grid grid-cols-1 md:grid-cols-2 gap-3 pt-2 border-t border-white/5">
          <label className="space-y-1 text-sm text-zinc-400">
            <span>{t('updates.repo_label', 'Upstream repository (owner/repo or release URL)')}</span>
            <input value={repo} onChange={e => setRepo(e.target.value)} placeholder={item.upstream || 'owner/repo'}
              className="w-full bg-black/50 border border-white/10 rounded-xl px-3 py-2 text-white" />
          </label>
          <label className="space-y-1 text-sm text-zinc-400">
            <span>{t('updates.asset_label', 'Release asset pattern (* matches anything)')}</span>
            <input value={asset} onChange={e => setAsset(e.target.value)} placeholder={item.pattern || 'payload-*.elf'}
              className="w-full bg-black/50 border border-white/10 rounded-xl px-3 py-2 text-white" />
          </label>
          <label className="flex items-center gap-3 text-sm text-zinc-300">
            <input type="checkbox" checked={item.prerelease} onChange={e => onPolicy(item, { prerelease: e.target.checked })} />
            {t('updates.prerelease_label', 'Include prereleases')}
          </label>
          <label className="flex items-center gap-3 text-sm text-zinc-300">
            <input type="checkbox" checked={item.unverified} onChange={e => onPolicy(item, { unverified: e.target.checked })} />
            {t('updates.unverified_label', 'Allow releases without a published checksum')}
          </label>
          <div className="md:col-span-2">
            <button onClick={() => onAdvanced(item, { repo: repo.trim(), asset: asset.trim() })}
              className="px-4 py-2 rounded-xl bg-white/10 hover:bg-ps-blue/20 font-bold text-sm">
              {t('updates.save_source', 'Save update source')}
            </button>
          </div>
        </div>
      )}
    </div>
  )
}

const UpdatesView = ({ onBack, addToast, showConfirm }) => {
  const { t } = useTranslation()
  const [status, setStatus] = useState(null)
  const [error, setError] = useState(false)
  const [busySlot, setBusySlot] = useState(null)
  const scopeRef = useRef(null)

  const load = useCallback(() => {
    scopeRef.current?.abort()
    const scope = createRequestScope()
    scopeRef.current = scope
    return apiFetch('/updates_status', { signal: scope.signal })
      .then(r => { if (!r.ok) throw new Error(String(r.status)); return r.json() })
      .then(data => { if (!scope.aborted) { setStatus(data); setError(false) } })
      .catch(() => { if (!scope.aborted) setError(true) })
  }, [])

  useEffect(() => {
    load()
    return () => scopeRef.current?.abort()
  }, [load])

  const running = status?.running
  useEffect(() => {
    if (!running) return
    const timer = setInterval(load, 1500)
    return () => clearInterval(timer)
  }, [running, load])

  const saveSettings = async (body, successMessage) => {
    try {
      const res = await mutationFetch('/updates_settings', {
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      })
      if (!res.ok) throw new Error(String(res.status))
      if (successMessage) addToast(successMessage)
    } catch {
      addToast(t('updates.save_failed', 'Could not save update settings'), 'error')
    }
    await load()
  }

  const savePayload = (item, changes, message) => saveSettings({ payloads: { [item.slot]: changes } }, message)

  const checkNow = async () => {
    try {
      const res = await mutationFetch('/updates_check')
      const data = await res.json()
      if (!data.ok) throw new Error('failed')
      if (!data.started) addToast(t('updates.check_running', 'A check is already running'))
    } catch {
      addToast(t('updates.check_failed', 'Could not start the update check'), 'error')
    }
    await load()
  }

  const runAction = async (item, route) => {
    setBusySlot(item.slot)
    try {
      const res = await mutationFetch(`${route}?slot=${encodeURIComponent(item.slot)}`)
      const data = await res.json()
      addToast(data.message || (data.ok ? t('updates.done', 'Done') : t('updates.failed', 'Failed')), data.ok ? 'success' : 'error')
    } catch {
      addToast(t('updates.request_failed', 'Request failed'), 'error')
    } finally {
      setBusySlot(null)
      await load()
    }
  }

  const rollback = item => showConfirm(
    t('updates.restore_title', 'Restore previous version'),
    t('updates.restore_message', 'Replace {{name}} {{current}} with {{previous}}? {{current}} will not be installed automatically again.',
      { name: item.name, current: item.installed || '?', previous: item.previous || '?' }),
    () => runAction(item, '/updates_rollback'))

  if (!status) {
    return (
      <div className="max-w-5xl mx-auto py-20 flex flex-col items-center gap-4 text-zinc-400">
        {error ? <AlertTriangle className="w-8 h-8 text-red-400" /> : <Loader2 className="w-8 h-8 animate-spin" />}
        <p>{error ? t('updates.load_failed', 'Could not load update status') : t('updates.loading', 'Loading…')}</p>
      </div>
    )
  }

  const channel = status.channel || {}
  const items = [...(status.items || [])].sort((a, b) => Number(b.autoload) - Number(a.autoload) || a.name.localeCompare(b.name))
  const lastRun = formatTime(status.last_run)

  return (
    <div className="max-w-5xl mx-auto space-y-10 pb-20">
      <div className="flex items-center space-x-6">
        <button onClick={onBack} aria-label={t('updates.back', 'Back')}
          className="p-4 rounded-2xl bg-white/5 hover:bg-white/10 transition-all border border-white/10">
          <ArrowLeft className="w-7 h-7" />
        </button>
        <h2 className="text-4xl font-extrabold text-white tracking-tight">{t('updates.title', 'Automatic Updates')}</h2>
      </div>

      <section className="grid grid-cols-1 xl:grid-cols-2 gap-6">
        <div className="p-5 md:p-8 bg-white/[0.03] rounded-3xl border border-white/10 space-y-6">
          <div className="flex items-start justify-between gap-4">
            <div className="space-y-1">
              <p className="font-bold text-white uppercase text-lg tracking-tight">{t('updates.enabled_title', 'Update payloads at startup')}</p>
              <p className="text-sm text-zinc-500 leading-relaxed">{t('updates.enabled_desc', 'Before each autoload payload starts, its maintainer\'s latest verified release is installed. Without network or on any error the installed version is used.')}</p>
            </div>
            <Toggle value={status.enabled} onChange={value => saveSettings({ enabled: value })} />
          </div>
          <div className="space-y-3">
            <p className="text-sm text-zinc-400">{t('updates.budget_desc', 'Longest time autoload waits for updates')}</p>
            <div className="grid grid-cols-4 gap-3">
              {BUDGETS.map(seconds => (
                <button key={seconds} onClick={() => saveSettings({ budget: seconds })}
                  className={cn('py-3 rounded-2xl font-black transition-all border',
                    status.budget === seconds ? 'bg-ps-blue border-ps-blue text-white' : 'bg-white/5 border-white/10 text-zinc-500 hover:text-white')}>
                  {seconds}s
                </button>
              ))}
            </div>
          </div>
          <div className="flex flex-wrap items-center gap-4">
            <button onClick={checkNow} disabled={running}
              className="inline-flex items-center gap-2 px-5 py-3 rounded-xl bg-white/10 hover:bg-ps-blue/20 font-bold disabled:opacity-60">
              {running ? <Loader2 className="w-5 h-5 animate-spin" /> : <RefreshCw className="w-5 h-5" />}
              {running ? t('updates.checking', 'Checking…') : t('updates.check_now', 'Check now')}
            </button>
            <span className="text-sm text-zinc-500 inline-flex items-center gap-2">
              {status.network === 'offline' && <CloudOff className="w-4 h-4" />}
              {status.network === 'offline' ? t('updates.offline', 'No network during the last check')
                : lastRun ? t('updates.last_check', 'Last check: {{time}}', { time: lastRun }) : null}
            </span>
          </div>
        </div>

        <div className="p-5 md:p-8 bg-white/[0.03] rounded-3xl border border-white/10 space-y-5">
          <div className="flex items-start justify-between gap-4">
            <div className="space-y-1">
              <p className="font-bold text-white uppercase text-lg tracking-tight flex items-center gap-2">
                <ShieldCheck className="w-5 h-5 text-ps-blue" />{t('updates.manager_title', 'Payload Manager')}
              </p>
              <p className="text-sm text-zinc-500 leading-relaxed">
                {t('updates.manager_desc', 'Updates only from its own release channel, so a custom build is never replaced by another one. New versions start the next time the manager is launched.')}
              </p>
            </div>
            {!channel.official && <Toggle value={status.self_update} onChange={value => saveSettings({ self_update: value })} />}
          </div>
          <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-2 text-sm">
            <dt className="text-zinc-500">{t('updates.channel', 'Channel')}</dt>
            <dd className="text-white font-mono break-all">{channel.repo}</dd>
            <dt className="text-zinc-500">{t('updates.running', 'Running')}</dt>
            <dd className="text-white">{channel.running}</dd>
            {channel.pending && <>
              <dt className="text-zinc-500">{t('updates.ready', 'Ready')}</dt>
              <dd className="text-emerald-300">{channel.pending}</dd>
            </>}
          </dl>
          {channel.message && (
            <p className={cn('text-sm flex items-start gap-2', channel.state === 'error' || channel.state === 'failed' ? 'text-amber-300' : 'text-zinc-400')}>
              {channel.state === 'ready' ? <CheckCircle2 className="w-4 h-4 mt-0.5 shrink-0" /> : null}
              {channel.message}
            </p>
          )}
        </div>
      </section>

      <section className="space-y-4">
        <h3 className="label-caps !text-ps-blue !opacity-100 flex items-center space-x-4 text-xl tracking-[0.2em]">
          <Package className="w-6 h-6" />
          <span>{t('updates.payloads_title', 'Installed payloads')}</span>
        </h3>
        {items.length === 0 && <p className="text-zinc-500">{t('updates.empty', 'No installed payloads yet.')}</p>}
        {items.map(item => (
          <PayloadRow key={`${item.slot}:${item.repo}:${item.asset}`} item={item} busy={busySlot === item.slot}
            onPolicy={(row, changes) => savePayload(row, changes)}
            onApply={row => runAction(row, '/updates_apply')}
            onRollback={rollback}
            onUnskip={row => savePayload(row, { skip: '' })}
            onAdvanced={(row, changes) => savePayload(row, changes, t('updates.source_saved', 'Update source saved'))} />
        ))}
      </section>
    </div>
  )
}

export default UpdatesView
