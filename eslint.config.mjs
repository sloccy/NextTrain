// ESLint flat config for src/pkjs/ (Pebble phone-side JS).
// Targets: 0 errors, 0 warnings.  Run: npx eslint src/pkjs/
// ES5 only — Pebble pkjs runtime constraint (no arrow fns, template literals, etc.).
// CommonJS sourceType — files are webpack-bundled; "type":"module" in package.json
//   would break the SDK's webpack.config.js (require() not available in ESM scope).
// Unused params/vars: prefix with _ to suppress (argsIgnorePattern / caughtErrorsIgnorePattern).
import js from "@eslint/js";

export default [
  js.configs.recommended,
  {
    files: ["src/pkjs/**/*.js"],
    languageOptions: {
      ecmaVersion: 5,
      sourceType: "commonjs",
      globals: {
        // Pebble JS SDK
        Pebble: "readonly",
        // Standard browser/JS globals available in Pebble pkjs
        XMLHttpRequest: "readonly",
        Uint8Array: "readonly",
        Int8Array: "readonly",
        localStorage: "readonly",
        console: "readonly",
        setTimeout: "readonly",
        clearTimeout: "readonly",
        setInterval: "readonly",
        clearInterval: "readonly",
        JSON: "readonly",
        Math: "readonly",
        Object: "readonly",
        Array: "readonly",
        Date: "readonly",
        parseInt: "readonly",
        parseFloat: "readonly",
        isNaN: "readonly",
        isFinite: "readonly",
        encodeURIComponent: "readonly",
        decodeURIComponent: "readonly",
        // CommonJS
        require: "readonly",
        module: "writable",
        exports: "writable",
        __dirname: "readonly",
        __filename: "readonly",
      },
    },
    rules: {
      "no-undef": "error",
      "no-unused-vars": ["error", { vars: "all", args: "after-used", argsIgnorePattern: "^_", caughtErrors: "all", caughtErrorsIgnorePattern: "^_" }],
      "no-unreachable": "error",
      "no-constant-condition": "error",
      "no-dupe-keys": "error",
      "no-duplicate-case": "error",
      "no-extra-semi": "error",
      "no-sparse-arrays": "error",
      "use-isnan": "error",
      "valid-typeof": "error",
      "eqeqeq": ["error", "always", { null: "ignore" }],
      "no-implied-eval": "error",
      "no-self-assign": "error",
      "no-self-compare": "error",
      "no-throw-literal": "error",
      "no-redeclare": ["error", { builtinGlobals: false }],
      "no-empty": ["error", { allowEmptyCatch: false }],

      "no-implicit-globals": "off",  // files are CommonJS modules, bundled by webpack
      "no-console": "off",
      "no-var": "off",
      "semi": ["warn", "always"],
      "no-trailing-spaces": "warn",
    },
  },
];
