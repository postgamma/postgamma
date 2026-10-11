export declare const isolationHeaders: Readonly<{
  'Cross-Origin-Opener-Policy': 'same-origin';
  'Cross-Origin-Embedder-Policy': 'require-corp';
}>;
export declare function copyAssets(destination: string, options?: {source?: string}): Promise<{
  buildId: string; directory: string; files: number;
}>;
