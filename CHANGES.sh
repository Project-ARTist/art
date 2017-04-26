#!/bin/sh
REVISION="7300ad4904bcf5f17d387d466708ee79cf893b5a"
echo ""
echo "Branch differs by Commits: #" `git log --oneline $REVISION.. | wc -l`
echo ""
git diff $REVISION
