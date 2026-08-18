---
layout: page
title: xspeccy-mcp
description: "Debugging ZX Spectrum code with an AI agent: the Xpeccy emulator core exposed as an MCP server."
---

`xspeccy-mcp` hands the emulator to an AI agent as a tool. The agent can load a
snapshot, run to a breakpoint, read memory, take a screenshot of the real frame
buffer and measure what a frame costs, so it checks its own Z80 code instead of
assuming it works.

The code, the build instructions and the list of tools are in the
[repository]({{ site.github.repository_url | default: 'https://github.com/alffcpu/xspeccy-mcp' }}).
These pages are the longer write-up: how to set a project up, what to keep in the
agent's notes, and how the debugging loop changes once the agent can run the
emulator itself.

## Read / Читать

- [ZX Spectrum, an AI agent, and debugging with xspeccy-mcp]({{ '/en/' | relative_url }}) - in English
- [ZX Spectrum, ИИ-агент и отладка через xspeccy-mcp]({{ '/ru/' | relative_url }}) - по-русски
