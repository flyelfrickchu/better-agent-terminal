// Locks how the GitHub panel interprets `gh pr list` / `gh issue list` results.
//
// The panel fetches both lists together. Before this, one shared error slot
// meant a repo with issues disabled (common on forks: gh exits non-zero with
// "the 'owner/repo' repository has disabled issues") blanked the PR tab too and
// showed a generic "Failed to fetch" with the real gh message thrown away.
// Each list now resolves independently, and "issues disabled" is a state, not
// an error.

import * as assert from 'assert'
import { resolveGitHubListResult } from '../renderer/src/utils/github-list-results.ts'

// ---- success: arrays pass through ----

const prs = resolveGitHubListResult<{ number: number }>([{ number: 1 }, { number: 2 }])
assert.deepStrictEqual(prs, { items: [{ number: 1 }, { number: 2 }], error: null, disabled: false })

const empty = resolveGitHubListResult([])
assert.deepStrictEqual(empty, { items: [], error: null, disabled: false })

// ---- issues disabled on the repo: empty list, no error ----

const disabled = resolveGitHubListResult({
  error: "the 'flyelfrickchu/better-agent-terminal' repository has disabled issues",
})
assert.deepStrictEqual(disabled, { items: [], error: null, disabled: true })

// ---- real failures keep gh's message ----

const failed = resolveGitHubListResult({ error: 'gh timed out' })
assert.deepStrictEqual(failed, { items: [], error: 'gh timed out', disabled: false })

const noRemote = resolveGitHubListResult({ error: 'no git remotes found' })
assert.strictEqual(noRemote.error, 'no git remotes found')

// ---- malformed responses are errors, not silent empties ----

assert.ok(resolveGitHubListResult(null).error)
assert.ok(resolveGitHubListResult(undefined).error)
assert.ok(resolveGitHubListResult({ foo: 1 }).error)
assert.ok(resolveGitHubListResult({ error: 42 }).error)

console.log('github-list-results: ok')
