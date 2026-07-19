# Issue tracker: GitHub

Issues and PRDs for this repo live in the GitHub repository
`Shadowfax-YJ/MaaAssistantArknights`. Use the `gh` CLI for all operations.

## Conventions

- Create: `gh issue create --repo Shadowfax-YJ/MaaAssistantArknights`
- Read: `gh issue view <number> --repo Shadowfax-YJ/MaaAssistantArknights --comments`
- List: `gh issue list --repo Shadowfax-YJ/MaaAssistantArknights`
- Comment: `gh issue comment <number> --repo Shadowfax-YJ/MaaAssistantArknights`
- Apply or remove labels: `gh issue edit <number> --repo Shadowfax-YJ/MaaAssistantArknights --add-label "..."` or `--remove-label "..."`
- Close: `gh issue close <number> --repo Shadowfax-YJ/MaaAssistantArknights --comment "..."`

## Pull requests as a triage surface

**PRs as a request surface: no.**

Do not include pull requests in the issue-triage queue. Collaborator and external pull requests remain outside this workflow.

## Skill operations

- “Publish to the issue tracker” means create a GitHub issue in this repository.
- “Fetch the relevant ticket” means read the GitHub issue and its comments.
- A bare `#42` may identify either an issue or pull request; resolve the type before acting.

## Wayfinding operations

A wayfinding map is one issue with child issues as tickets.

- Label maps `wayfinder:map`.
- Label children `wayfinder:<type>`, where type is `research`, `prototype`, `grilling`, or `task`.
- Prefer GitHub sub-issues and native issue dependencies.
- If those features are unavailable, use a task list in the map, `Part of #<map>` in each child, and `Blocked by: #<number>` for dependencies.
- Claim a ticket by assigning it to the active GitHub user.
- Resolve it by commenting with the answer, closing it, and adding a context pointer to the map’s Decisions-so-far section.
