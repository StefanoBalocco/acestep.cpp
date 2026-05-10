<script lang="ts">
	import type { Snippet } from 'svelte';

	let {
		open = $bindable(false),
		title,
		body,
		actions,
		onConfirm
	}: {
		open: boolean;
		title: string;
		body?: Snippet;
		actions?: Snippet<[() => void]>;
		onConfirm?: () => void;
	} = $props();

	let root = $state<HTMLDivElement>();

	function cancel() {
		open = false;
	}

	function confirm() {
		open = false;
		onConfirm?.();
	}

	// Same outside-click pattern as Menu.svelte. Listeners attach only while
	// open so closed dialogs cost nothing globally. Escape always cancels.
	// Enter confirms only when the default Cancel/OK footer is in use; with a
	// custom actions snippet the user picks an explicit button.
	$effect(() => {
		if (!open) return;
		const onMouseDown = (e: MouseEvent) => {
			if (!root?.contains(e.target as Node)) cancel();
		};
		const onKey = (e: KeyboardEvent) => {
			if (e.key === 'Escape') cancel();
			else if (e.key === 'Enter' && !actions) confirm();
		};
		document.addEventListener('mousedown', onMouseDown);
		document.addEventListener('keydown', onKey);
		return () => {
			document.removeEventListener('mousedown', onMouseDown);
			document.removeEventListener('keydown', onKey);
		};
	});
</script>

{#if open}
	<div class="dialog-overlay">
		<div class="dialog" bind:this={root}>
			<div class="dialog-title">{title}</div>
			{#if body}
				<div class="dialog-body">{@render body()}</div>
			{/if}
			<div class="dialog-footer">
				{#if actions}
					{@render actions(cancel)}
				{:else}
					<button type="button" class="dialog-btn" onclick={cancel}>Cancel</button>
					<button type="button" class="dialog-btn" onclick={confirm}>OK</button>
				{/if}
			</div>
		</div>
	</div>
{/if}

<style>
	.dialog-overlay {
		position: fixed;
		inset: 0;
		background: rgba(0, 0, 0, 0.5);
		display: flex;
		align-items: center;
		justify-content: center;
		z-index: 100;
	}
	.dialog {
		background: var(--bg-card);
		border-radius: 4px;
		box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
		padding: 0.75rem;
		min-width: 16rem;
		display: flex;
		flex-direction: column;
		gap: 0.5rem;
	}
	.dialog-title {
		font-size: 0.85rem;
		color: var(--fg);
	}
	.dialog-body {
		font-size: 0.8rem;
		color: var(--fg);
	}
	.dialog-footer {
		display: flex;
		justify-content: flex-end;
		gap: 0.4rem;
	}
	/* Global so the actions snippet, authored in the parent component,
	   reuses the exact same button look without duplicating styles.
	   Doubled class selector raises specificity to 0,0,2,0 so the rule
	   wins over a scoped `button { ... }` in the parent (which Svelte
	   compiles to `button.svelte-HASH`, specificity 0,0,1,1). */
	:global(.dialog-btn.dialog-btn) {
		background: var(--bg-btn);
		border: none;
		cursor: pointer;
		padding: 0.2rem 0.6rem;
		color: var(--fg);
		font-size: 0.8rem;
		border-radius: 3px;
	}
	:global(.dialog-btn.dialog-btn:hover) {
		background: var(--bg-btn-hover);
	}
</style>
