// UserPromptSubmit hook: when the conversation's context grows past a threshold,
// remind Claude (and the user) to update docs/VR_HANDOFF.md and move on to a
// new session. Fires once at the first threshold, then again every STEP tokens.
//
// Context size = the last assistant turn's input + cache read + cache write
// tokens, read from the tail of the transcript.

const fs = require( 'fs' );
const os = require( 'os' );
const path = require( 'path' );

const FIRST = 150000;
const STEP = 100000;
const TAIL_BYTES = 4 * 1024 * 1024;

function readStdin()
{
	try { return JSON.parse( fs.readFileSync( 0, 'utf8' ) ); } catch { return {}; }
}

function lastContextTokens( transcript )
{
	const fd = fs.openSync( transcript, 'r' );
	try
	{
		const size = fs.fstatSync( fd ).size;
		const len = Math.min( size, TAIL_BYTES );
		const buf = Buffer.alloc( len );
		fs.readSync( fd, buf, 0, len, size - len );
		const lines = buf.toString( 'utf8' ).split( '\n' );
		for( let i = lines.length - 1; i >= 0; i-- )
		{
			if( !lines[ i ].includes( '"usage"' ) ) continue;
			try
			{
				const u = JSON.parse( lines[ i ] )?.message?.usage;
				if( !u ) continue;
				return ( u.input_tokens || 0 ) + ( u.cache_read_input_tokens || 0 ) +
					( u.cache_creation_input_tokens || 0 );
			}
			catch { /* partial first line of the tail */ }
		}
	}
	finally { fs.closeSync( fd ); }
	return 0;
}

function main()
{
	const input = readStdin();
	if( !input.transcript_path || !fs.existsSync( input.transcript_path ) ) return;

	const tokens = lastContextTokens( input.transcript_path );
	if( tokens < FIRST ) return;

	const bucket = Math.floor( ( tokens - FIRST ) / STEP );
	const stateFile = path.join( os.tmpdir(), `psuex-session-size-${ input.session_id || 'x' }.txt` );
	let last = -1;
	try { last = parseInt( fs.readFileSync( stateFile, 'utf8' ), 10 ); } catch { }
	if( bucket <= last ) return;
	try { fs.writeFileSync( stateFile, String( bucket ) ); } catch { }

	const k = Math.round( tokens / 1000 );
	process.stdout.write( JSON.stringify( {
		systemMessage: `Context is ~${ k }k tokens. Consider updating docs/VR_HANDOFF.md and starting a new session.`,
		hookSpecificOutput: {
			hookEventName: 'UserPromptSubmit',
			additionalContext:
				`Session size reminder: this conversation's context is ~${ k }k tokens, and every turn re-sends it. ` +
				'Per the "Sessions and token usage" rule in CLAUDE.md: answer the current request, then tell the user in one line ' +
				'that the session is getting large. If the next request is a new topic, update docs/VR_HANDOFF.md with the current ' +
				'state first, then propose continuing in a new session (start_session) or /clear.'
		}
	} ) );
}

try { main(); } catch { /* never block a prompt */ }
