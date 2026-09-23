// Interprets one `github.listPRs` / `github.listIssues` response.
//
// The host returns either the parsed gh JSON array or `{ error: string }`.
// Each list resolves on its own so a failure in one (e.g. issues disabled on a
// fork, where `gh issue list` exits non-zero) never hides the other.

export interface GitHubListResult<T> {
  items: T[]
  error: string | null
  // The repo has the feature turned off (currently: issues). Not an error.
  disabled: boolean
}

const DISABLED_PATTERN = /repository has disabled issues/i

export function resolveGitHubListResult<T>(result: unknown): GitHubListResult<T> {
  if (Array.isArray(result)) {
    return { items: result as T[], error: null, disabled: false }
  }
  if (result && typeof result === 'object' && 'error' in result) {
    const raw = (result as { error: unknown }).error
    const message = typeof raw === 'string' && raw.trim() ? raw.trim() : 'unknown gh error'
    if (DISABLED_PATTERN.test(message)) {
      return { items: [], error: null, disabled: true }
    }
    return { items: [], error: message, disabled: false }
  }
  return { items: [], error: 'unexpected response from gh', disabled: false }
}
