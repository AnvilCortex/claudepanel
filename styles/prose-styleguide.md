# Prose styleguide

Voice and judgment rules for delivered prose: documentation, client-facing material, proposals, slides, executive summaries, and any other writing that leaves the team. The deterministic rules live in the Vale config under `styles/Voice/`, and CI runs them on changed lines. This file covers what a linter cannot check: register, sentence rhythm, vocabulary, and the patterns that need a human read.

Working-voice chat between teammates, ticket prose, and internal planning notes are out of scope. Those have their own conventions and do not need a published styleguide.

## Writing for the reader

A document is a finished thing written for the person reading it. Decide who that reader is and what they should be able to do when they finish, then match everything to them.

Executives and clients want outcomes and decisions in plain language. Practitioners want the inputs, the steps, and what to expect, in the order they will do them. Engineers want interfaces, data shapes, edge cases, and real names and values. For a mixed audience, write the main path for the least technical reader and move depth into labeled sections they can skip. When the audience is unclear, state the assumption you are writing to.

Lead with the answer. Open with what the reader came for, the definition, the steps, the decision, or the result, and put the context underneath. A reader should get the core in the first few lines and stop early when that is all they needed.

Match depth to the reader. Use the plainest language that stays accurate, and define a term the first time a non-expert would trip on it. Keep database columns, code identifiers, file paths, and internal code names out of writing for non-technical readers. Be concrete for engineers.

Keep the document about its subject. Explain the thing itself. How the document was produced, the reasoning behind it, and the decision history are not part of it.

Show, then tell. A concrete example, a short sequence of steps, or a small table often lands the idea faster than prose. Pick a real number or a real name over the abstract. One worked example is enough unless a second example shows a structurally different case. A second example that varies surface details without changing the structure adds noise.

Structure for scanning. Use headings, short paragraphs, and plain lists so a reader can find a section and skim it. Headings are sentence case. Let the page or folder layout carry the organization instead of repeating it in prose.

## Write for the reader's context

The writer accumulates context as they work: sibling documents, prior conversations, codebases skimmed for reference, internal vocabulary picked up along the way. The reader has none of it. Their context is this page.

Drift happens when the writer mistakes their accumulated context for shared context. Vocabulary creeps in without definition. Cross-references multiply. Concepts that feel explained somewhere else get compressed or skipped. Early writing is clean because the writer's context is small. Later writing gets denser as the context grows.

Name concepts here with enough framing to stand on their own. When pointing to another source, link to the canonical place instead of recapping it. If a single sentence of local framing is not enough to land the concept, either explain it here or send the reader to the canonical place first.

## Default shape

Open with the subject. Add the context that matters. Stop.

Use medium sentences joined with "and", "so", or "but" when the clauses are genuinely linked. Keep sentences readable instead of chopping everything into tiny polished fragments.

Capitalize normally. Lowercase "i" and lowercase sentence starts do not belong in delivered prose.

## No signposting

Cut prose that narrates the act of reading instead of contributing meaning.

- "Read this for...", "Start here to...", "Use this when...", "See this for...", "Refer to this for..." in an index or table of contents are pointer-doubling. The link already points; the gloss should define.
- "This page covers...", "This section explains...", "In this document we will..." restate the heading. Cut them.
- A heading followed by a one-line restatement of the heading is the same move. Write the content directly.
- Refer to other content by name, not by position. "Section 3 above", "the next chapter", "as described earlier" force the reader to chase positions that move when content moves. Name the concept directly (the auth flow, the rate-limiter, the applier) or drop the pointer if it adds nothing.

A clean check: remove the leading clause ("Read this for", "This section explains"). If the sentence still means the same thing, the leading clause was signposting and the sentence is better without it.

Index entries are short noun-phrase definitions of the linked concern, the way a glossary reads, not click-decision prose.

## Sentence rules

Use lists for requirements, review findings, or a small set of options. Keep bullets concrete. Avoid decorative list labels and bolded mini-headings.

Ask real questions only when the question is open. Rhetorical questions in delivered prose read as setup.

Use "is" and "has" when they are enough. "The page has a routing bug" is more direct than "the page serves as a manifestation of a routing issue".

No "X is what makes Y" cleft constructions. Write "X makes Y" or "Without X, Y stops working". The cleft adds a beat without adding meaning, and it leans toward the kind of clever-sounding generalization that ages badly.

No synonym cycling. Name a concept once and reuse the same word. Renaming it every sentence (the system, the platform, the engine, the runtime) to dodge repetition reads as variety theater, and the reader has to work to track that all four words point at one thing.

No false ranges. "From X to Y" implies X and Y sit on a comparable scale. "From draft to published" works because the scale is one document's state. "From security to scalability" does not, because those are different categories, not endpoints of one axis. If the endpoints are not on the same axis, write a list.

No generic upbeat endings. A closing sentence that promises a bright future, exciting times, or transformative impact in place of saying what happens next is filler. End with the concrete next step, the open question, or stop.

## Vocabulary to avoid

Vale enforces most of this list, so a flagged word is a prompt to pick a plainer one. The rule file holds the full set; a few examples for each:

- Politeness boilerplate like `Certainly`, `Great question`, or `I hope this helps`. (Voice.ChatbotPoliteness)
- Abstract importance words like `pivotal`, `robust`, `seamless`, or `tapestry`. (Voice.AbstractImportance)
- Promotional language like `vibrant`, `nestled`, or `groundbreaking`. (Voice.Promotional)
- Fake authority like `experts agree` or `it is important to note`. (Voice.FakeAuthority)
- Inflated transitions like `furthermore`, `at its core`, or `the real question is`. (Voice.InflatedTransitions)
- Signposting like `let's dive in` or `here's what you need to know`. (Voice.Signposting)
- Filler that compresses to one word, like `in order to` or `due to the fact that`. (Voice.FillerPhrases, Voice.FillerVerbs)
- Copula avoidance like `serves as` or `stands as`. Prefer "is" or "has". (Voice.CopulaAvoidance)

A few tells are not linted because they have honest literal uses, like `landscape` as an abstract noun or figurative `rich`. Watch for them on the read.

## Hard dislikes

The rules marked level: error are the hard bans; CI annotates them on changed lines, and the rest are a manual read.

No em dashes. Use commas, periods, or parentheses. (Voice.EmDash, level: error.)

No corrective contrast built on a negated setup. The pattern pairs an opener like `not only`, `not just`, `isn't just`, or `it's not` with a following contrast clause. Write two plain statements instead. (Voice.NegativeParallelism, level: error.) The bare "this is X, not Y" shape is the same move but is not linted, because factual contrasts ("red, not blue") are fine, so catch the rhetorical ones on the read.

Straight quotes only. No curly or smart quotes. (Voice.CurlyQuotes, level: error.)

No emojis in prose. (Voice.Emoji.)

No forced groups of three. If there are two real points, write two. If there are five real requirements, list five. This includes three-word "impact" lines ("faster, simpler, cleaner"). A linter cannot judge this, so catch it on the read.

No counts of categories, including the hidden form. Stating that a system has "three deployment modes" or "four authentication methods" turns a description into a quiz answer the reader has to remember. The universal-quantifier form is the same move in disguise: "every request carries a token", "all writes go through one path", "each user gets a role". Drop the quantifier and let the noun phrase do the work ("the token requests carry").

Do not name a set by its count. Phrases like "the seven service tiers", "the five core principles", or "the four pipeline stages" promote a starting cardinality into a contract, and every downstream writer then defends the number ("five principles, not six") instead of describing the parts. Today's starting set is tomorrow's grown set. Name the set by what it is (the service tiers, the core principles, the pipeline stages) and let the parts speak for themselves.

No inline-header bullet lists, the kind that open each item with a bolded mini-heading and a colon. Write prose or a plain list. Vale cannot see this once markdown is parsed, so it is a manual check.

No verbose backstory or meta-commentary in reference docs. Explain the concept and the current decision. The research trail, changelog, generation notes, "how to read this" sections, source-file pointers, and decision history stay out of the artifact.

No restating adjacent content. If a paragraph after a table re-explains the rows, the table already carried the commitment and the paragraph is redundant. The same applies to two adjacent paragraphs where the second restates the first in different words, a bullet whose second sentence carries rationale that belongs in a sibling section, or a section that re-argues a point a neighboring section already made. Each section, paragraph, and bullet owns its claim once.

No padded Markdown tables or ASCII-art tables. If a table is necessary, use the minimal separator (`|-|-|`), not padded runs of hyphens (`|---|---|`). Box-drawing line art is an error-level finding. (Voice.BoxDrawing, level: error.)

No tables for glossary content. A glossary is a list of terms with their definitions, and lists render that natively. If a term needs more than its definition (status, owner, category, link), carry the extras as chips, inline labels, or a small structured layout under each entry, not as table columns. Tables are for content that is genuinely tabular, where the rows and columns share comparable scales.

## Examples

### Client email

Before:

"This initiative represents a significant opportunity to improve operational efficiency across the organization. The proposed approach will enhance visibility and support stronger collaboration."

After:

"The proposal lets the team split the work into smaller pieces while keeping one view of status and ownership. That should make the work easier to assign and to track."

### Slide copy

Before:

"The platform provides a comprehensive solution for managing complex enterprise workflows."

After:

"The platform gives teams one place to define the work and assign owners. It also shows status and what still needs attention."

### Technical explanation

Before:

"Static typing is a powerful software development paradigm that improves code reliability by enabling compile-time validation of data structures."

After:

"Static typing means TypeScript checks the shape of your data before the code runs. If a function expects a number and you pass text, it catches the mistake while you are still building."

### Index entry

Before:

"[Rate limiting](./rate-limiting.md). Read this for how the gateway throttles incoming requests when traffic exceeds the configured threshold."

After:

"[Rate limiting](./rate-limiting.md). The gateway's request-throttling behavior when traffic exceeds the configured threshold."
