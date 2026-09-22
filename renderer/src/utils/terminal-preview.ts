// Bounded terminal preview text for TerminalThumbnail.
//
// The thumbnail keeps only the last few lines of PTY output. It must stay
// bounded in *bytes*, not just lines: build tools redraw progress bars with
// `\r` and never emit `\n`, so line-count trimming alone lets one line grow
// without limit (and every chunk re-scans the whole thing).

export const PREVIEW_MAX_LINES = 8
export const PREVIEW_MAX_LINE_CHARS = 512

// Strip all ANSI escape sequences and problematic characters
export function stripAnsi(str: string): string {
  return str
    // CSI sequences: \x1b[ followed by params and command char
    .replace(/\x1b\[[0-9;?]*[A-Za-z]/g, '')
    // OSC sequences: \x1b] ... (terminated by BEL \x07 or ST \x1b\\)
    .replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g, '')
    // Other escape sequences: \x1b followed by single char
    .replace(/\x1b[()][AB012]/g, '')
    .replace(/\x1b[=>]/g, '')
    // DCS, PM, APC sequences
    .replace(/\x1b[PX^_][^\x1b]*\x1b\\/g, '')
    // Bell character
    .replace(/\x07/g, '')
    // Carriage return (often used for overwriting lines)
    .replace(/\r/g, '')
    // Any remaining single-char escapes
    .replace(/\x1b./g, '')
    // Private Use Area characters (Powerline, Nerd Fonts icons) - causes box characters
    .replace(/[-]/g, '')
    // Braille patterns (often used for terminal graphics)
    .replace(/[⠀-⣿]/g, '')
    // Box drawing characters that may not render well at small sizes
    .replace(/[─-╿]/g, '')
}

/**
 * Append a raw PTY chunk to an existing (already bounded) preview and return
 * the new bounded preview: at most PREVIEW_MAX_LINES lines, each clipped to
 * its last PREVIEW_MAX_LINE_CHARS characters.
 *
 * Cost per call is O(prev + data). `prev` is bounded by this function's own
 * output, so total work never grows with the amount of output seen so far.
 */
export function appendTerminalPreview(prev: string, data: string): string {
  const cleaned = stripAnsi(prev + data)
  const lines = cleaned.split('\n').slice(-PREVIEW_MAX_LINES)
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].length > PREVIEW_MAX_LINE_CHARS) {
      lines[i] = lines[i].slice(-PREVIEW_MAX_LINE_CHARS)
    }
  }
  return lines.join('\n')
}
