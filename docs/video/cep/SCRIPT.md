# CEP video narration (about 2:00)

Paste everything under "Narration" into ElevenLabs as one block.

## Narration

Imagine asking your university's AI assistant: "What's the last date for course registration?"

And it tells you, very confidently... the tenth of September.

But that date was changed. It's actually the seventeenth.

So what went wrong? The AI wasn't broken. It just read the wrong notice.

Here's the thing. An AI can only read so much text at once. We measure that space in tokens. For one question, our search finds around forty passages, but only about nine will fit. So, somebody has to decide: which nine?

Think of it like packing a small bag for a trip. Your passport goes in first. Then you pick what's actually useful. You don't pack the same shirt twice, and you definitely don't pack last year's ticket.

That's exactly what our Context Budget Manager does.

First, it throws out any notice that a newer one has replaced. Next, it scores what's left, and it likes new facts, trusted sources, and short text. Then it keeps picking the best one, until the budget is full. Never over.

To make this work, we built four data structures ourselves. A binary heap hands us the best passage next. Merge sort keeps things in order. A linked list holds the final package. And a hash map remembers what we've already seen.

Okay, let's see it run. We pick a question, set the budget to five hundred and twelve tokens, and compare all four methods. Look at the last row. That's ours. Both facts found, no old notices, no copies.

Now, back to our registration question, with just one hundred and twenty-eight tokens. One method still says the tenth. Ours says the seventeenth.

We tested this on thirty-two questions. At five twelve tokens, it found ninety-one percent of the facts we needed, more than any other method. All twenty-nine tests pass. And in over twenty-four thousand runs, it never went over budget. Not once.

Is it perfect? No. It's a little slower, it uses more memory, and we still need to try it with a real AI model.

But here's what we learned. A smart assistant isn't just about how much it can read. It's about choosing what to read.

Thanks for watching.
