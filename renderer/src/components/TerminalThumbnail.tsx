import { host } from '../host-api'
import { useEffect, useState, memo } from 'react'
import type { TerminalInstance } from '../types'
import { ActivityIndicator } from './ActivityIndicator'
import { settingsStore } from '../stores/settings-store'
import { getAgentPreset } from '../types/agent-presets'
import { appendTerminalPreview } from '../utils/terminal-preview'

// Global preview cache - persists across component unmounts
const MAX_PREVIEW_CACHE = 100
const previewCache = new Map<string, string>()
const previewSubscribers = new Map<string, Set<() => void>>()

/** Remove a terminal's preview from the cache (call when terminal is destroyed) */
export function clearPreviewCache(terminalId: string) {
  previewCache.delete(terminalId)
  previewSubscribers.delete(terminalId)
}

function updatePreviewCache(id: string, value: string) {
  previewCache.set(id, value)
  previewSubscribers.get(id)?.forEach(fn => fn())
}

function subscribeToPreview(id: string, fn: () => void): () => void {
  if (!previewSubscribers.has(id)) previewSubscribers.set(id, new Set())
  previewSubscribers.get(id)!.add(fn)
  return () => {
    const subs = previewSubscribers.get(id)
    if (!subs) return
    subs.delete(fn)
    if (subs.size === 0) previewSubscribers.delete(id)
  }
}


// Global listener setup - only once
let globalListenerSetup = false
const setupGlobalListener = () => {
  if (globalListenerSetup) return
  globalListenerSetup = true

  // Evict oldest entries if cache is too large
  const evictIfNeeded = () => {
    if (previewCache.size > MAX_PREVIEW_CACHE) {
      const firstKey = previewCache.keys().next().value
      if (firstKey) previewCache.delete(firstKey)
    }
  }

  // PTY output for regular terminals
  host.pty.onOutput((id, data) => {
    // Bounded in lines *and* bytes: CR-only progress bars must not grow a
    // single line forever (see utils/terminal-preview.ts).
    updatePreviewCache(id, appendTerminalPreview(previewCache.get(id) || '', data))
    evictIfNeeded()
  })

  // Claude agent messages for agent terminal previews
  host.claude.onMessage((sessionId, message) => {
    const msg = message as { role?: string; content?: string }
    if (msg.role === 'assistant' && msg.content) {
      const lines = msg.content.split('\n').slice(-8)
      updatePreviewCache(sessionId, lines.join('\n'))
    }
  })

  // Claude agent streaming text for live preview
  host.claude.onStream((sessionId, data) => {
    const stream = data as { text?: string }
    if (stream.text) {
      updatePreviewCache(sessionId, appendTerminalPreview(previewCache.get(sessionId) || '', stream.text))
    }
  })
}

interface TerminalThumbnailProps {
  terminal: TerminalInstance
  isActive: boolean
  onClick?: () => void
}

export const TerminalThumbnail = memo(function TerminalThumbnail({ terminal, isActive, onClick }: TerminalThumbnailProps) {
  const [preview, setPreview] = useState<string>(previewCache.get(terminal.id) || '')
  const [fontFamily, setFontFamily] = useState<string>(settingsStore.getFontFamilyString())

  // Check if this is an agent terminal
  const isAgent = terminal.agentPreset && terminal.agentPreset !== 'none'
  const agentConfig = isAgent ? getAgentPreset(terminal.agentPreset!) : null
  const isWorktreeTerminal = !!terminal.worktreePath
  const displayTitle = terminal.alias || terminal.title

  useEffect(() => {
    setupGlobalListener()

    // Subscribe to cache updates for this terminal (event-driven, no polling)
    const unsubscribePreview = subscribeToPreview(terminal.id, () => {
      setPreview(previewCache.get(terminal.id) || '')
    })

    // Subscribe to settings changes for font updates
    const unsubscribeSettings = settingsStore.subscribe(() => {
      setFontFamily(settingsStore.getFontFamilyString())
    })

    return () => {
      unsubscribePreview()
      unsubscribeSettings()
    }
  }, [terminal.id])

  return (
    <div
      className={`thumbnail ${isActive ? 'active' : ''} ${isAgent ? 'agent-terminal' : ''}`}
      onClick={onClick}
      title={terminal.title}
      style={agentConfig ? { '--agent-color': agentConfig.color } as React.CSSProperties : undefined}
    >
      <div className="thumbnail-header">
        <div className={`thumbnail-title ${isAgent ? 'agent-terminal' : ''}`}>
          {isAgent && <span>{agentConfig?.icon}</span>}
          {isWorktreeTerminal && <span title={terminal.worktreeBranch || 'worktree'}>🌳</span>}
          <span>{displayTitle}</span>
        </div>
        <ActivityIndicator terminalId={terminal.id} size="small" />
      </div>
      <div className="thumbnail-preview" style={{ fontFamily }}>
        {preview || (isAgent ? '' : '$ _')}
      </div>
    </div>
  )
})
