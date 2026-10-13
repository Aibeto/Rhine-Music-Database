import tsParser from '@typescript-eslint/parser';

// 只检查自维护脚本；上游、依赖和生成文件保留原样。
export default [
    {
        ignores: ['node_modules/**', 'output/**', 'vendor/**', 'MusicBridge/upstream/**'],
    },
    {
        files: [
            'Qt/logic/**/*.js',
            'Qt/tests/**/*.{mjs,cjs}',
            'MusicBridge/*.cjs',
            'scripts/*.{cjs,mjs}',
            'Scene/**/*.ts',
            'Resources/**/*.ts',
            'eslint.config.mjs',
        ],
        languageOptions: {
            parser: tsParser,
            ecmaVersion: 'latest',
            sourceType: 'module',
        },
        rules: {
            curly: ['error', 'all'],
            'one-var': ['error', 'never'],
            'no-nested-ternary': 'error',
            'no-sequences': ['error', { allowInParentheses: false }],
            'no-empty': 'error',
        },
    },
];
