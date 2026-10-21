# Modern theme, LanguageTool and the Writing Assistant

This version of LyX adds:

* a **modern theme**: quiet, paper-like chrome (light and dark) and a centered
  text column;
* **LanguageTool** checking while you type: spelling (red underline) and
  grammar/style (blue underline);
* a **Writing Assistant** side panel to chat with any OpenAI-compatible model
  about your document, with optional web search through **SearXNG**;
* a setting for the folder of your **LaTeX distribution** (TeX Live or MiKTeX).

All settings are in *Tools > Preferences*.

## Setup

### LaTeX distribution

*Language Settings > Writing Assistant & LaTeX > LaTeX Distribution*:
choose the installation folder (e.g. `C:\Program Files\MiKTeX`,
`C:\texlive\2025`, `/usr/local/texlive/2025`) or directly the folder that
contains `pdflatex`. LyX finds the programs folder (`bin/<platform>` for
TeX Live, `miktex/bin/x64` for MiKTeX) and adds it to the `PATH`.
The label below the field shows what was found. After changing it, use
*Tools > Reconfigure* and restart LyX.

### LanguageTool

1. *Language Settings > Writing Assistant & LaTeX*: enter the server URL,
   for example `http://100.70.61.20:8010/v2` (the `/v2` may be omitted).
   *Test Connection* checks it.
2. *Language Settings > Spellchecker*: choose
   **LanguageTool (spelling and grammar)** as engine and keep
   *Spellcheck continuously* on.

Paragraphs are sent to the server when you pause typing (about 0.7 s).
Math, citations, references and other insets are sent as markup, so
sentences are checked as a whole. Right-click an underlined word or phrase
for LanguageTool's explanation and suggestions. *Add to personal dictionary*
uses the same word lists as the other spellcheckers.
*Tools > Spellchecker* (F7) also works; it waits for the server's answers.

### Writing Assistant (AI)

*Language Settings > Writing Assistant & LaTeX > AI Assistant*:

* **API base URL**: the URL up to and including `/v1`, e.g.
  `http://localhost:8080/v1` (llama.cpp), `http://localhost:11434/v1`
  (Ollama), `http://localhost:1234/v1` (LM Studio),
  `https://api.openai.com/v1`.
* **API key**: only if your server needs one. It is stored in plain text in
  the LyX preferences file.
* **Model**: *Fetch Models* lists what the server offers; leave empty to use
  the first one.

Open the panel with *View > Writing Assistant Pane*, the toolbar button or
**F8**.

* Type a message and press Enter (Shift+Enter for a new line).
* The combo box chooses what is sent with your message: the selection or the
  current paragraph (default), the whole document, or nothing.
* *Actions* offers one-click tasks: proofread, explain mistakes, improve
  clarity, shorten, expand, change tone, continue writing, summarize,
  suggest titles, translate.
* *Insert* puts the answer at the cursor, *Replace* replaces the text the
  request was about (after a rewrite action), *Copy* copies it. Undo works as
  usual. Math and other insets in replaced text come back as plain text.

### Web search (SearXNG)

Enter the SearXNG URL (e.g. `http://localhost:8888`). JSON output must be
enabled in SearXNG's `settings.yml`:

```yaml
search:
  formats:
    - html
    - json
```

Switch on **Web** in the panel. Models that support tool calling decide
themselves when to search (`web_search`) and which pages to read
(`fetch_url`). For servers or models without tool calling, LyX searches
for your question first and gives the results to the model.

### Theme

*Look & Feel > User Interface > Theme*: *Modern* (follows the system
light/dark setting), *Modern Light*, *Modern Dark* or *Classic* (the
previous look). The stylesheet is `lib/ui/modern.qss`; copy it into the `ui`
folder of your LyX user directory to customize it. The text column width is
set in *Editing > Limit text width*. Work area colors can still be changed
in *Look & Feel > Colors*.
