import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import {
  IMAGE_API_HOSTS,
  IMAGE_SOURCE_HOSTS,
} from "./src/lib/image-providers";

function normalizeBase(value: string): string {
  const withLeadingSlash = value.startsWith("/") ? value : `/${value}`;
  return withLeadingSlash.endsWith("/")
    ? withLeadingSlash
    : `${withLeadingSlash}/`;
}

function httpsSources(hosts: readonly string[]): string {
  return hosts.map((host) => `https://${host}`).join(" ");
}

function contentSecurityPolicy(): string {
  const apiSources = httpsSources(IMAGE_API_HOSTS);
  const imageSources = httpsSources(IMAGE_SOURCE_HOSTS);
  return [
    "default-src 'self'",
    "base-uri 'self'",
    `connect-src 'self'${apiSources ? ` ${apiSources}` : ""}`,
    "font-src 'self'",
    `img-src 'self'${imageSources ? ` ${imageSources}` : ""}`,
    "manifest-src 'self'",
    "media-src 'none'",
    "object-src 'none'",
    "script-src 'self'",
    "style-src 'self' 'unsafe-inline'",
    "worker-src 'self'",
    "form-action 'self'",
  ].join("; ");
}

function contentSecurityPolicyPlugin() {
  return {
    name: "arachne-content-security-policy",
    transformIndexHtml: {
      order: "pre" as const,
      handler() {
        return [
          {
            tag: "meta",
            attrs: {
              "http-equiv": "Content-Security-Policy",
              content: contentSecurityPolicy(),
            },
            injectTo: "head-prepend" as const,
          },
        ];
      },
    },
  };
}

export default defineConfig(({ command }) => ({
  // The development server needs Vite's HMR transport. Production HTML gets
  // the registry-derived, closed network policy used by the static viewer.
  plugins: [react(), ...(command === "build" ? [contentSecurityPolicyPlugin()] : [])],
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
