"""Stage the original PC file-based save worker for the Web service backend.

Preserves the original request classes and save serialization. This worker is
not itself a platform service. Mutation callbacks now require a completed
browser storage commit; actual game/persistence behavior remains unverified.
"""
import hashlib, json, re
from pathlib import Path

ROOT=Path(__file__).resolve().parent
SOURCE=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src')
MODULE=SOURCE/'win32/gameServicesWin'
records=[]
for name,output_name in [('gameServicesWinWorker.h','game-services-worker-web.h'),('gameServicesWinWorker.cpp','game-services-worker-web.cpp')]:
    source=MODULE/'src'/name
    text=source.read_text(encoding='utf-8-sig').replace('GameServicesWinWorker','GameServicesWebWorker')
    def include(match):
        target=match[1]
        if target=='gameServicesWinWorker.h': return '#include "game-services-worker-web.h"'
        if target=='gameServicesWin.h': return '#include "common/gameServices/include/gameServices.h"'
        if target=='build.h': return '#include "common/core/include/corePublic.h"\n#include "common/gameServices/include/gameServicesInternal.h"'
        candidates=[source.parent/target,MODULE/target,SOURCE/target]
        resolved=next((p.resolve() for p in candidates if p.is_file()),None)
        if resolved is None: raise RuntimeError('Unresolved original include: '+target)
        # Resolve through the compiler's original source-root include directory.
        # Absolute slash paths on Windows produce mixed-separator nested paths
        # that can miss Clang's exact VFS overlay redirects.
        return '#include "'+resolved.relative_to(SOURCE).as_posix()+'"'
    text=re.sub(r'#include "([^"]+)"',include,text)
    if name.endswith('.h'):
        if text.count('Bool m_exit;')!=1: raise RuntimeError('Original worker exit flag changed')
        text=text.replace('Bool m_exit;','red::Atomic<Bool> m_exit;')
    # Windows long is 32-bit; Wasm64 long is 64-bit. These four format arguments
    # are the original 32-bit MetadataVersion enum or Int32 loop index.
    if name.endswith('.cpp'):
        text=text.replace('while ( !m_exit )','while ( !m_exit.GetValue() )')
        text=text.replace('if ( m_exit )','if ( m_exit.GetValue() )')
        text=text.replace('m_exit = true;', 'RED_SCOPE_LOCK(m_workerMutex);\n\t\tm_exit.SetValue(true);')
        # Use one predicate mutex for both enqueue and sleep. The original PC
        # worker could lose a WakeAll between finding an empty queue and waiting.
        text=re.sub(r'^\t\tRED_SCOPE_LOCK\( m_requestsLock \);', '\t\tRED_SCOPE_LOCK(m_workerMutex);\n\t\tRED_SCOPE_LOCK( m_requestsLock );',text,flags=re.M)
        wait='\t\t\tRED_SCOPE_LOCK( m_workerMutex );\n\t\t\tm_wakeUpWorker.Wait( m_workerMutex );'
        if text.count(wait)!=1:raise RuntimeError('Original worker wait changed')
        text=text.replace(wait,'''\t\t\tRED_SCOPE_LOCK( m_workerMutex );
\t\t\t{
\t\t\t\tRED_SCOPE_LOCK(m_requestsLock);
\t\t\t\tif (!m_requests.Empty() || m_exit.GetValue()) continue;
\t\t\t}
\t\t\tm_wakeUpWorker.Wait( m_workerMutex );''')
        text='#include "persistence-web.h"\n'+text
        persist_helper='''
namespace services {
static ServiceError CommitWebMutation() {
    return cp2077::web::FlushPersistentStorage() ? ServiceError_OK : ServiceError_InternalError;
}
}
'''
        text=text.replace('namespace\n{',persist_helper+'\nnamespace\n{',1)
        if text.count('metadata.%ld.json')!=4: raise RuntimeError('Metadata format sites changed')
        text=text.replace('metadata.%ld.json','metadata.%d.json')
        # GameSaveView carries a full path only on Windows. Keep the existing
        # Web ABI: all these paths are already local variables, and name/id
        # identify the same save for the original callbacks.
        if text.count('saveView.path =')!=3: raise RuntimeError('Save path assignment sites changed')
        text=re.sub(r'^\s*saveView\.path = (save|saveFilePath|saveFile);\n', '\n', text, flags=re.M)
        text=text.replace('saveView.path.AsChar(), saveView.path.Length()', 'save.AsChar(), save.Length()')
        original_load='''\t\tred::AbsolutePath saveFile;
\t\tif ( !metadata->m_path.Empty() )
\t\t{
\t\t\tsaveFile = metadata->m_path;
\t\t}
\t\telse
\t\t{
\t\t\tsaveFile = red::paths::GetSaveDirectory().AddDirPath( metadata->m_name );
\t\t\tsaveFile.AppendFilePath( c_saveFilename );
\t\t}'''
        if text.count(original_load)!=1: raise RuntimeError('Original load path selection changed')
        text=text.replace(original_load, '\t\tauto saveFile = red::paths::GetSaveDirectory().AddDirPath( metadata->m_name );\n\t\tsaveFile.AppendFilePath( c_saveFilename );')
        for callback in ['onFinished( saveView, ServiceError_OK )','onRemoved( saveView, ServiceError_OK )','onRemoved( ServiceError_OK )','onCreated( ServiceError_OK )','onUploaded( saveView, ServiceError_OK )']:
            if callback not in text: raise RuntimeError('Mutation completion site changed: '+callback)
            text=text.replace(callback,callback.replace('ServiceError_OK','CommitWebMutation()'))
        # Flush the original buffered screenshot writers before snapshotting VFS
        # bytes. Do not destroy them inside the renderer completion callback.
        screenshot_save='\t\t\t\tonFinished( saveView, CommitWebMutation() );\n\t\t\t\tisFinished.SetValue( true );\n\t\t\t};'
        if text.count(screenshot_save)!=1: raise RuntimeError('Save screenshot completion changed')
        text=text.replace(screenshot_save,'\t\t\t\tm_screenshotFile->Flush();\n'+screenshot_save)
        screenshot_export='\t\t\t\t\tonCreated( CommitWebMutation() );'
        if text.count(screenshot_export)!=1: raise RuntimeError('Screenshot export completion changed')
        text=text.replace(screenshot_export,'\t\t\t\t\tscreenshotFile->Flush();\n'+screenshot_export)
        text=text.replace(screenshot_save, screenshot_save.replace('CommitWebMutation()', 'm_screenshotFile->HasErrors() ? ServiceError_InternalError : CommitWebMutation()'))
        text=text.replace(screenshot_export, screenshot_export.replace('CommitWebMutation()', 'screenshotFile->HasErrors() ? ServiceError_InternalError : CommitWebMutation()'))
        screenshot_start='\t\t// write screenshot\n\t\t{'
        if text.count(screenshot_start)!=1: raise RuntimeError('Save screenshot stage changed')
        text=text.replace(screenshot_start,'''\t\t// Initialize save identity even when an optional screenshot cannot be created.
\t\tsaveView.id = red::CalculatePathHash32(saveFilePath.AsChar(), saveFilePath.Length());
\t\tsaveView.date = GFileManager->GetFileTime(saveFilePath);
'''+screenshot_start)
        serialized='\t\t\tfile->Serialize( buffer.Data(), metadata->m_fileSize );'
        if text.count(serialized)!=1: raise RuntimeError('Save serialization site changed')
        text=text.replace(serialized,serialized+'''\n\t\t\tfile->Flush();
\t\t\tif (file->HasErrors()) {
\t\t\t\tonFinished(saveView, ServiceError_InternalError);
\t\t\t\tisFinished.SetValue(true);
\t\t\t\treturn;
\t\t\t}''')
        metadata_write='\t\t\tred::SaveStringToFile( metaFile, outString );'
        if text.count(metadata_write)!=1: raise RuntimeError('Metadata write site changed')
        text=text.replace(metadata_write,'''\t\t\tif (!red::SaveStringToFile(metaFile, outString)) {
\t\t\t\tonFinished(saveView, ServiceError_InternalError);
\t\t\t\tisFinished.SetValue(true);
\t\t\t\treturn;
\t\t\t}''')
    output=ROOT/'src'/output_name
    if not output.is_file() or output.read_text(encoding='utf-8')!=text:
        output.write_text(text,encoding='utf-8')
    records.append({'OriginalSource':str(source),'OriginalSHA256':hashlib.sha256(source.read_bytes()).hexdigest(),'AdaptedSource':str(output),'AdaptedSHA256':hashlib.sha256(output.read_bytes()).hexdigest(),'Adaptations':['Web worker name','original include resolution','32-bit metadata format arguments','save name/id instead of Windows-only absolute-path fields','browser storage completion before mutation acknowledgement','check serialized save and metadata writes']})
(ROOT/'build/save-worker-source-manifest.json').write_text(json.dumps(records,indent=2))
print('Staged original file-based save worker; platform service integration remains required')
