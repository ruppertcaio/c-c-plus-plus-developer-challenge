# Dynamox C/C++ Developer Challenge

In order to contribute to the enhancement of Dynamox solutions, we present you with the following challenge:

**Using C and/or C++, design and implement a message packetizer for device-to-device communication, and demonstrate it working end to end.**

While going through the challenge, you should be able to handle ambiguous situations, adhere to best practices in embedded firmware development, and demonstrate problem-solving skills. Effective communication through well-documented code, code quality, readability, and maintainability will also be evaluated.

---

## Challenge: Message Packetizer 📦

### Overview

Picture an **IoT device with limited resources** that continuously exchanges messages and data blocks with other devices. The links between them are byte-oriented and cannot always be trusted: bytes may arrive corrupted, in fragments, duplicated, or not arrive at all.

In this challenge you will design and implement, **from scratch**, the layer that makes this communication trustworthy: a **packetizer**, a library that takes application messages on one side, turns them into packets suitable for transmission, and reconstructs the original messages on the other side.

There is no starter code. Every design decision (how a packet looks on the wire, how problems are detected and handled, how the pieces fit together) is yours to make, to implement, and to justify.

It is not mandatory to fulfill all requirements: prioritize what best demonstrates your skills in the time you have available. Feel free to make any assumptions you consider necessary; documenting them counts in your favor.

### 1 - User Stories

1. [ ] As a user, I want to send **any message** (text or binary, from a few bytes to much larger than a single packet) and receive it intact in another process.
2. [ ] As a user, I want to know whether each message was delivered or failed.
3. [ ] As a user, I want the communication to survive a misbehaving link (data lost, corrupted or duplicated): corrupted data is never delivered as valid, and the applications keep working without restart.
4. [ ] As a user, I want to send **any file** and receive it byte-identical, with its integrity verified upon arrival.

### 2 - Technical Requirements

1. [ ] Use C, C++ or both.
2. [ ] The packetizer core must be a **standalone, transport-agnostic library**, written for the device described in the overview.
3. [ ] Provide applications running as **two separate processes** (dedicated sender/receiver, or one peer app launched twice) that communicate through your library over a transport **of your choice**.
4. [ ] The applications must accept **arbitrary input at runtime** (a message we type, a file path we pass).
5. [ ] Document your packet format and the reasoning behind it.
6. [ ] Cover the packetizer core with automated unit tests.
7. [ ] Provide a build system (Makefile, CMake, ...) and instructions to build and run everything on Linux.

### 3 - Bonus

1. [ ] Demonstrate your communication surviving a hostile channel: inject loss, corruption and duplication (e.g. a channel simulator or test harness) and show messages still arriving correctly.
2. [ ] Full-duplex operation: both sides can send and receive **at the same time**. For example, messages keep flowing in both directions while a file transfer is in progress, without one blocking the other.
3. [ ] Support a second, different transport using the same packetizer core, demonstrating that the abstraction holds.
4. [ ] Measure and report your protocol's overhead and throughput, and discuss the trade-offs of your design.
5. [ ] File transfer conveniences: progress reporting, transfer of multiple files, resuming an interrupted transfer.

<br>

## Evaluation Criteria

Each one of the items above will be evaluated as "Not Implemented", "Implemented with Issues", "Implemented", or "Implemented with Excellence". In order to assess different profiles and experiences, we expect candidates applying to more senior levels to demonstrate a deeper understanding of the requirements and implement more of them in the same deadline.

In general we will be looking for the following:

1. [ ] Anyone should be able to follow the instructions and build/run the applications.
2. [ ] User stories implemented according to the requirements.
3. [ ] Protocol design: your packet format, the mechanisms you chose, and how it could evolve.
4. [ ] Problem-solving skills and ability to handle ambiguity.
5. [ ] Best practices in embedded firmware development, including resource awareness.
6. [ ] Code quality, readability, and maintainability.
7. [ ] Tests that give real confidence under adverse conditions.
8. [ ] Git usage: small commits with meaningful messages.

## Ready to Begin the Challenge?

* Fork this repository to your own GitHub account.
* Create a new branch using your first name and last name. For example: `caroline-oliveira`.
* After completing the challenge, create a pull request to this repository (https://github.com/dynamox-s-a/c-c-plus-plus-developer-challenge), aimed at the main branch. Describe your design and your assumptions in the pull request description.
* We will receive a notification about your pull request, review your solution, and get in touch with you.

<br>

**Good luck! We look forward to reviewing your submission.** 🚀

## Frequently Asked Questions

* Is it necessary to fork the project?
  **Yes, this allows us to see how much time you spent on the challenge.**

* Can I use third-party libraries?
  **For the transport layer and tooling, yes. The packetizer core (packet format, integrity, message reconstruction, delivery control) must be entirely your own code.**

* Can I use AI to complete the challenge?
  **Yes, however keep in mind you will need to explain your decisions and your code.**

* If I have more questions, who can I contact?
  **Please reply to the email that sent you this test.**