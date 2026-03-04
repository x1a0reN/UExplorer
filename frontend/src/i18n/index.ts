// i18n configuration
import { translations } from './translations';

export type Language = 'zh' | 'en';

const STORAGE_KEY = 'uexplorer-language';

// Get saved language or default to Chinese
export function getLanguage(): Language {
  if (typeof window === 'undefined') return 'zh';
  const saved = localStorage.getItem(STORAGE_KEY);
  if (saved === 'en' || saved === 'zh') return saved;
  return 'zh';
}

// Set language and save to localStorage
export function setLanguage(lang: Language): void {
  localStorage.setItem(STORAGE_KEY, lang);
}

// Translation function
export function t(key: string): string {
  const lang = getLanguage();
  const dict = translations[lang];
  return dict[key as keyof typeof dict] || key;
}

// Get translations for current language
export function getTranslations() {
  return translations[getLanguage()];
}
