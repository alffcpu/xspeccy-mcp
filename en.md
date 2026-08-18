---
layout: page
title: "ZX Spectrum, an AI agent, and debugging with xspeccy-mcp"
description: "How to use xspeccy-mcp so an AI agent can run, test, and optimize ZX Spectrum code on its own."
lang: en
permalink: /en/
---

[Русская версия]({{ '/ru/' | relative_url }})

## A short preface

I became interested in the Spectrum demoscene in the second half of the 1990s. The scene had existed before, of course, but I think it became much more visible in Russia after the first Enlight parties. My first demoparty was Chaos Constructions'00. After that I spent several years taking part in different scene events.

Back then many of us still worked on real Spectrum machines. Emulators were not yet a normal part of the process. The pain of debugging and all the other suffering that came with making demos felt natural and unavoidable. Later, real machines became harder to find and working on a modern PC became more convenient. Editors improved and emulators got more features, but debugging still took a lot of time.

Over the years I followed the scene less and started writing software mostly for money. But I still enjoy development. Especially when I manage to take a good idea all the way to a finished result. Programming is still my hobby. I just no longer want to spend all my free time on exhausting debugging and endless optimization.

The tools changed too. At one point computers were programmed with switches and punch cards. It is a rough comparison, but the point is clear. Then programming languages appeared. Now we have tools that can write code with us or for us. To me, this is simply another way to program. There are plenty of arguments around it, but this article is not about them.

Like many people, I do not like AI-slop. There is already enough low-quality rubbish made only because it can now be made quickly. I also do not like the habit of calling everyone who actively uses AI in development a vibe coder. The term has already been spoiled by people who wanted to make quick money from it.

What I suggest here has little to do with vibe-coding. Many of us spent years learning the Spectrum and know it better than the technologies we are paid to use at work. That knowledge helps us give precise tasks and tell a good result from a random one. I only suggest looking at familiar development from another angle and trying to make something with almost no manual coding.

I wanted to try this approach specifically in the Spectrum demoscene. There is almost no commercial work here, so these tools are not going to fire or replace anyone. We spend this time on ourselves. You remain yourself, but now you have the combined abilities of sair00s, poisoned cyberjack, and TMK at hand. Why not leave more time for the creative part?

## What this is about

The idea is simple: spend less time thinking about every line of Z80 code and more time on the idea, the structure of the project, and what should appear on the screen in the end.

This does not mean that you can know nothing about the platform and hand everything to the agent. The better you know the Spectrum, the more precisely you can describe the task and judge the result. But you can greatly reduce the most tiring part of the work: building, running, testing ideas, and finding bugs in machine code.

This is where `xspeccy-mcp` comes in. It is an MCP server built around the Xpeccy emulator core. It gives an AI agent access to the emulator as a tool: load a snapshot, run code, take a screenshot, set a breakpoint, read memory, measure a frame, and collect a profile. The agent no longer has to assume that its code should work. It can test the code in the real emulator core.

## Start with a small knowledge base

An agent's context is not infinite. During a long task it gets compacted, and some details are lost. The agent may forget where a table lives, which bank is currently paged in, or why a particular address must not be used.

It is better to put the main information in the repository from the start. There is no need for a complex system. A few Markdown files with clear names and a short index are enough:

- details of the target machine: model, memory size, timing, and interrupts;
- the memory map and the purpose of each bank;
- build and run instructions;
- notes about graphics, music, and resource formats;
- known limitations and problems.

This is the working memory of the project, not another Z80 manual. It should contain more than final decisions. It is useful to record why an optimization was rejected or why an apparently free memory area turned out to be in use.

After context compaction, the agent can read these files again and continue. They can be extended and reused in similar projects. They also help the human developer. A week later there is no need to reconstruct the whole process from commits.

Do not dump the whole internet into this knowledge base. A few short files with exact rules are more useful than one huge document. For external information, a link and the date when it was checked are usually enough.

## Planning: from simple parts to the complete project

There is no new development method here. First comes the idea, then a list of parts, separate prototypes, and finally the complete project. A person or a regular team works in much the same way.

For this experiment, it is better not to start with code. Describe the picture, movement, memory limits, approximate update rate, music, and scene order. Then ask the agent to split the task into parts and ask questions about anything that is still unclear.

You could say that you temporarily take the roles of product owner and architect. That sounds too formal for a hobby project, but the meaning is simple. You decide what the result should be, what parts it needs, and in what order they should be built.

Instead of asking for "a nice fast effect", give the agent something concrete. For example: a tunnel with a double-buffered screen, no pre-rendered video, a specific palette, a memory limit, and a clear amount of frame headroom. On the Spectrum these limits matter. "Nice and fast" is not a technical specification.

## Connecting xspeccy-mcp

You need to clone `xspeccy-mcp`, build it once, and connect it to the agent environment. The repository provides `python3 build.py --smoke` for this. It downloads a supported Xpeccy version, builds the server, and runs the tests. You can also point it at Xpeccy sources that are already present. On Windows the build needs MinGW-w64, not MSVC.

Xpeccy changes over time, so it is better to use the version specified by the `xspeccy-mcp` repository. Moving to another version may require changes to the integration. An agent can study those changes, but the tests and a real project snapshot should be checked again afterwards.

The server runs without a window and is not tied to real time. Once connected, the agent can run the build and the emulator as many times as needed.

## How development changes

The cycle stays the same: edit, build, run, and check. The difference is that the agent can now complete the cycle itself and immediately work on the problem it found.

The most useful `xspeccy-mcp` operations for this are:

- loading a snapshot and running for a given number of frames or until a label;
- breakpoints, registers, memory reads and writes, and disassembly;
- PNG output from the real frame buffer and GIF recording;
- `screen_digest` for comparing attributes or the full screen memory before and after a change;
- `frame_cost` for measuring frame cost, interrupt count, budget, and headroom;
- `profile` for splitting execution time by code region;
- labels from the `sjasmplus` symbol file and source-level stepping after loading a `.lst` file.

A screenshot is saved to a file, which the agent can then open and inspect. `screen_digest` does not replace looking at the result, but it quickly shows whether the screen bytes changed during the frames being checked.

Measurements come from the Xpeccy core. Interrupts, time spent in `halt`, frame geometry, and the selected machine are taken into account. Before measuring anything, explicitly select the machine model, ROM set, and screen configuration. During this project, a wrong ROM set once broke working effects during initialization.

Many effects build tables before their first frame. If measurement starts too early, the profile includes initialization and the screenshot may still be black. First run to `main_loop`, or to your own main-loop label, and save a new snapshot. Later measurements can start from that snapshot.

A working result is not necessarily the result the author wants. You still have to look at the scene and adjust its speed, shape, color, or logic. The tool handles much of the manual code checking, but it does not make the creative decisions.

## Memory and speed

In a Spectrum project you need to know where the code, screens, tables, buffers, graphics, and music live. Moving one part can overwrite another, and sometimes the error only appears after several frames.

It is better to keep a memory map from the beginning. Code size can be taken from the listing, data areas from declarations in the source, and addresses can be checked against the symbol file. The state after initialization can then be checked by reading memory through MCP. This can reveal something that is not obvious from the source alone. A table that looks temporary may still be needed during the show.

Speed work follows the same order. First measure with `frame_cost`, then use `profile` to find the expensive regions. Make one small change, measure again, and compare the screen. If the numbers improve and the picture stays the same, keep the change. Otherwise revert it and record the reason in the log.

Counting T-states by hand is still useful during planning. The final result should be measured on the assembled code in the emulator. Even time spent in `halt`, or crossing one more interrupt boundary, can change the conclusion about an optimization.

## Use case: a demo

When integration started, this demo already had two working parts: nine separate scenes and a tunnel background. Before combining them, both parts were built with a listing. The listing was used to count the code, while the data areas were taken from the sources. Then `frame_cost` measured the background and every scene, and `profile` split the background frame by routine. After that, a map of all eight memory banks was prepared.

The result was clear: everything fit in memory, but it did not fit in the frame budget. The background did not leave enough headroom for music, compositing, and the heaviest scene. So the bank layout and compositing method were chosen before work on the combined engine began.

The first combined frame used the simplest version: a separate interrupt and paging module, two buffers for the mask and image, and a separate compositing pass. The first step was to check that it worked correctly. The frozen background matched the original memory dumps, and a long run passed more than two thousand frames. After that, compositing was merged into the background output and the texture layout was changed. The frame became about 33,000 T-states cheaper, while `screen_digest` still matched across three complete interlace cycles.

The music was compressed with ZX0, and every byte was compared after decompression. A separate placement bug was also found: at first the compressed block occupied tables that were built during initialization. After this was fixed, scenes were added according to the scenario table and working steps were saved as separate commits. Finally, the whole demo was run from start to finish and its synchronization was checked by interrupt numbers.

As a general example, the order is simple: get the separate parts working, collect sizes, timings, and a memory map, make the simplest combined version, verify it, and optimize only after that.

## Logs are part of the result

Ask the agent to keep a development log from the start. It is enough to record the goal of a change, the decision, measurements, the verification method, rejected options, and the next step. This work used one common `DEVLOG` and separate logs for prototypes. They showed what had already been measured, what had been reverted, and why.

The log should not be a retelling of the conversation. It should contain the facts that a person or an agent will need to continue after a break.

## Conclusion

I see a lot of potential here for the Spectrum scene. The limits of standard hardware are broadly understood by now, along with the main ways to approach them. The interesting part is no longer who can spin another cube a little faster. New results are more likely to come from an idea: a form, presentation, an unusual technique, or a combination of effects.

Understanding the platform is still required. The Spectrum still does not forgive mistakes with memory, timing, or interrupts. But with `xspeccy-mcp`, it has become a little kinder.

You provide the idea and the limits. The agent writes and checks the code. The hours spent finding three bytes that overwrote the next table can now be spent on the demo itself.

---

Sources and setup: [github.com/alffcpu/xspeccy-mcp](https://github.com/alffcpu/xspeccy-mcp)

The demo mentioned above: [CPU, DOCKS, U](https://github.com/alffcpu/cpu-docks-u-zx-demo)
