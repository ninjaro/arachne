import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

function normalizeBase(value: string): string {
  const withLeadingSlash = value.startsWith("/") ? value : `/${value}`;
  return withLeadingSlash.endsWith("/")
    ? withLeadingSlash
    : `${withLeadingSlash}/`;
}

export default defineConfig(({ command }) => ({
  plugins: [react()],
  base:
    command === "serve"
      ? "/"
      : normalizeBase(process.env.ARACHNE_VIEWER_BASE ?? "/arachne/viewer/"),
  publicDir: "public",
  build: {
    outDir: "dist",
    emptyOutDir: true,
  },
}));
