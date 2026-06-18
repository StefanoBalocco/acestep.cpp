#!/bin/bash

cd tools/webui
rm -rf node_modules
pnpm install
pnpm run format
pnpm run lint
pnpm run check
pnpm run build
